'use strict';

const $ = id => document.getElementById(id);
const post = message => window.chrome.webview.postMessage(JSON.stringify(message));

const state = {
  mode: 'chat',
  activeSession: '',
  sessions: [],
  models: [],
  activeModel: '',
  hardware: { vram_mb: 0, ram_mb: 0 },
  modelLoaded: false,
  busy: false,
  agents: [],
  resources: [],
  tools: [],
  activeAgent: null,
  attachments: [],
  pendingUser: null,
  streaming: null,
  pendingConfig: null,
  selectedAgentType: 'local',
  settings: {},
  settingsDetected: {},
  followOutput: true,
  recording: false,
  transcribing: false,
  mediaRecorder: null,
  mediaStream: null,
  audioChunks: [],
  transcriptionJob: 0
};

const transcript = $('transcript');

function route(message) {
  const handler = handlers[message.type];
  if (handler) handler(message);
}
window.chrome.webview.addEventListener('message', event => route(event.data));

const handlers = {
  status(message) {
    state.modelLoaded = !!message.model_loaded;
    state.activeModel = message.active_model_id || state.activeModel;
    state.busy = !!message.busy;
    $('model-led').className = 'status-dot' + (state.modelLoaded ? ' loaded' : '');
    const model = state.models.find(item => item.id === state.activeModel);
    $('model-status-text').textContent = state.modelLoaded
      ? `${model?.name || 'Local model'} · ${Number(message.n_ctx || 0).toLocaleString()} ctx`
      : 'No model loaded';
    $('unload-model').hidden = !state.modelLoaded;
    syncModelSelections();
    updateComposerState();
  },

  model_loading(message) {
    if (message.id) state.activeModel = message.id;
    syncModelSelections();
    $('model-led').className = 'status-dot loading';
    $('model-status-text').textContent = 'Loading model…';
  },

  hardware_info(message) {
    state.hardware = { vram_mb: message.vram_mb || 0, ram_mb: message.ram_mb || 0 };
    const el = $('hw-detected');
    if (!el) return;
    if (!state.hardware.vram_mb && !state.hardware.ram_mb) {
      el.textContent = 'Could not detect GPU/RAM automatically. Presets below use conservative defaults \u2014 adjust manually below if needed.';
      return;
    }
    const gb = mb => (mb / 1024).toFixed(mb % 1024 === 0 ? 0 : 1);
    el.textContent = `Detected ${gb(state.hardware.vram_mb)} GB VRAM, ${gb(state.hardware.ram_mb)} GB system RAM. `
      + 'Presets below are scaled to this machine.';
  },

  models(message) {
    state.models = Array.isArray(message.list) ? message.list : [];
    state.activeModel = message.active || state.activeModel;
    renderModels();
    renderModelManageList();
  },

  model_added(message) {
    state.activeModel = message.id;
    toast('Model added. Loading it now.');
    post({ type: 'load_model', id: message.id });
  },

  sessions(message) {
    state.sessions = Array.isArray(message.list) ? message.list : [];
    if (message.active) state.activeSession = message.active;
    renderSessions();
  },

  history(message) {
    if (message.session_id !== state.activeSession) return;
    state.streaming = null;
    transcript.replaceChildren();
    for (const item of message.messages || []) {
      if (item.role === 'user') addMessage('user', item.content);
      else if (item.role === 'assistant' && !item.tool_name) addMessage('assistant', item.content);
      else if (item.role === 'assistant') addToolChip('TOOL', `${item.tool_name} ${item.content}`, 'call');
      else if (item.tool_name === 'conversation_summary') addNote(`Earlier conversation compressed to: ${item.content}`);
      else addToolChip('RESULT', `[${item.tool_name}] ${item.content}`, 'result');
    }
    renderEmptyState();
    state.followOutput = true;
    scrollTranscript(true);
  },

  workspace(message) {
    state.agents = Array.isArray(message.agents) ? message.agents : [];
    state.resources = Array.isArray(message.resources) ? message.resources : [];
    // A rename while the agent is open must refresh the held reference too, or
    // the banner/title keeps showing the name from when it was opened.
    if (state.activeAgent) {
      const fresh = state.agents.find(a => a.id === state.activeAgent.id);
      if (fresh) { state.activeAgent = fresh; syncComposerAgent(); }
    }
    renderAgents();
    renderResources();
    renderAgentRagOptions();
  },

  tools(message) {
    state.tools = Array.isArray(message.list) ? message.list : [];
    renderBuiltInTools();
  },

  settings(message) {
    state.settings = message.values || {};
    state.settingsDetected = message.detected || {};
    populateSettings();
    renderBuiltInTools();
  },

  settings_saved(message) {
    toast(message.reloading ? 'Settings saved. Reloading the active model.' : 'Settings saved.');
    closeSettings();
  },

  transcription_started(message) {
    state.transcribing = true;
    state.transcriptionJob = Number(message.id || 0);
    updateMicState('Transcribing…');
  },

  transcription_progress(message) {
    if (state.transcriptionJob && Number(message.id || 0) !== state.transcriptionJob) return;
    updateMicState(message.note || `Transcribing ${Number(message.progress || 0)}%`);
  },

  transcription_result(message) {
    if (state.transcriptionJob && Number(message.id || 0) !== state.transcriptionJob) return;
    state.transcribing = false;
    state.transcriptionJob = 0;
    insertTranscriptText((message.text || '').trim());
    updateMicState();
  },

  transcription_error(message) {
    state.transcribing = false;
    state.transcriptionJob = 0;
    updateMicState();
    toast(message.message || 'Whisper transcription failed.', true);
  },

  files_picked(message) {
    const list = Array.isArray(message.list) ? message.list : [];
    if (message.kind === 'attachment') {
      for (const resource of list) {
        if (!state.attachments.some(item => item.id === resource.id)) state.attachments.push(resource);
      }
      renderAttachments();
      return;
    }
    if (message.kind === 'agent_config' && list[0]) {
      state.pendingConfig = list[0];
      $('agent-config-name').textContent = list[0].name;
      return;
    }
    if (message.kind === 'rag') toast(`${list.length} file${list.length === 1 ? '' : 's'} added to RAG.`);
    if (message.kind === 'tool_pack') toast('Tool manifest imported.');
  },

  agent_created() {
    closeTaskModal();
    toast('Agent created.');
  },

  agent_opened(message) {
    if (message.batch !== undefined) { const b=$('stop-agent'); if(b) b.hidden=false; }
    state.activeSession = message.session_id;
    state.activeAgent = state.agents.find(agent => agent.id === message.agent_id) || null;
    if (state.activeAgent?.model_id && state.activeAgent.model_id !== state.activeModel) {
      post({ type: 'load_model', id: state.activeAgent.model_id });
    }
    showAgentChat();
    syncComposerAgent();
  },

  turn_accepted(message) {
    if (message.session_id !== state.activeSession) return;
    state.busy = true;
    if (state.pendingUser) state.pendingUser.classList.remove('pending');
    state.pendingUser = null;
    state.attachments = [];
    renderAttachments();
    updateComposerState();
  },

  turn_rejected(message) {
    if (message.session_id === state.activeSession && state.pendingUser) state.pendingUser.remove();
    state.pendingUser = null;
    state.busy = false;
    toast(message.message || 'Turn rejected.', true);
    updateComposerState();
    renderEmptyState();
  },

  gen_started(message) {
    state.busy = true;
    updateComposerState();
    if (message.session_id !== state.activeSession) return;
    removeEmptyState();
    state.streaming = addMessage('assistant', '', false, false);
    state.streaming.querySelector('.message-text').classList.add('cursor');
  },

  token(message) {
    if (message.session_id !== state.activeSession || !state.streaming) return;
    state.streaming._raw = (state.streaming._raw || '') + (message.text || '');
    scheduleStreamRender();
  },

  // Private reasoning: its own channel, collapsed by default.
  thinking(message) {
    if (message.session_id !== state.activeSession || !state.streaming) return;
    appendThinking(state.streaming, message.text || '');
    scrollTranscript(false);
  },

  // The model narrating a tool call. Arrives before the call itself.
  note(message) {
    if (message.session_id !== state.activeSession) return;
    if (!state.streaming) return;
    state.streaming._note = (state.streaming._note || '') + (message.text || '');
  },

  assistant_final(message) {
    if (message.session_id !== state.activeSession) return;
    cancelStreamRender();
    if (state.streaming) {
      const item = state.streaming;
      const text = item.querySelector('.message-text');
      text.classList.remove('cursor');
      item._raw = message.text || '';
      setMessageText(item, item._raw, 'assistant');
      if (message.thinking) {
        const pane = thinkingPane(item);
        const bodyEl = pane.querySelector('.thinking-body');
        if (!bodyEl.textContent) {
          bodyEl.textContent = message.thinking;
          const words = message.thinking.trim().split(/\s+/).filter(Boolean).length;
          pane.querySelector('.thinking-meta').textContent = words ? `${words} words` : '';
        }
      }
      state.streaming = null;
    } else {
      addMessage('assistant', message.text || '');
    }
    scrollTranscript(false);
  },

  tool_call(message) {
    if (message.session_id !== state.activeSession) return;
    cancelStreamRender();
    const note = message.note || state.streaming?._note || '';
    const thinking = message.thinking || '';
    if (state.streaming) {
      // Keep the bubble only if it carried reasoning worth showing.
      const pane = state.streaming.querySelector('.thinking');
      if (pane && !thinking) {
        state.streaming.querySelector('.message-text').remove();
      } else {
        state.streaming.remove();
      }
      state.streaming = null;
    }
    if (note) addNote(note);
    addToolChip('TOOL', `${message.name} ${JSON.stringify(message.args || {})}`, 'call');
  },

  operator_result(message) {
    if (message.session_id && message.session_id !== state.activeSession) return;
    addToolChip(message.ok ? '/' + message.name : 'ERROR', message.text || '', message.ok ? 'result' : 'call');
  },

  memory(message) {
    state.memory = message;
    const box = $('memory-text');
    if (box && document.activeElement !== box) box.value = message.text || '';
    const meter = $('memory-meter');
    if (meter) {
      meter.textContent = `${message.bytes} / ${message.budget} bytes`;
      meter.classList.toggle('over', message.bytes > message.budget * 0.9);
    }
  },

  memory_saved(message) {
    toast(message.message || (message.ok ? 'Memory saved.' : 'Could not save memory.'), !message.ok);
  },

  tool_result(message) {
    if (message.session_id !== state.activeSession) return;
    addToolChip('RESULT', `[${message.name}] ${message.result || ''}`, 'result');
  },

  context_usage(message) {
    if (message.session_id && message.session_id !== state.activeSession) return;
    const meter = $('ctx-meter');
    if (!meter) return;
    const used = message.used || 0, budget = message.budget || 1;
    const nctx = message.n_ctx || budget, fixed = message.fixed || 0;
    const pctUsed = Math.min(100, Math.round((used / budget) * 100));
    const pctFixed = Math.min(100, Math.round((fixed / budget) * 100));
    $('ctx-fill').style.width = pctUsed + '%';
    $('ctx-fixed').style.width = pctFixed + '%';
    meter.classList.toggle('warn', pctUsed >= 75 && pctUsed < 100);
    meter.classList.toggle('over', pctUsed >= 100);
    const k = n => n >= 1000 ? (n / 1000).toFixed(n >= 10000 ? 0 : 1) + 'k' : String(n);
    $('ctx-text').textContent =
      `${k(used)} / ${k(budget)} tokens (${pctUsed}%) · ${k(fixed)} fixed · ${k(nctx)} ctx`;
    meter.hidden = false;
  },

  job_update(message) {
    if (message.session_id !== state.activeSession) return;
    renderJob(message);
  },

  cancelled(message) {
    if (message.session_id === state.activeSession) {
      if (state.streaming) {
        state.streaming.querySelector('.message-text').classList.remove('cursor');
        state.streaming = null;
      }
      addToolChip('STOPPED', 'Generation cancelled.', 'error');
    }
  },

  error(message) {
    if (message.session_id && message.session_id !== state.activeSession) {
      toast(message.message || 'Background operation failed.', true);
      return;
    }
    if (state.streaming) {
      state.streaming.querySelector('.message-text').classList.remove('cursor');
      state.streaming = null;
    }
    if (message.session_id) addToolChip('ERROR', message.message || 'Unknown error', 'error');
    else toast(message.message || 'Unknown error', true);
    state.pendingUser = null;
    state.busy = false;
    syncModelSelections();
    updateComposerState();
  },

  turn_done() {
    // Nothing can still be running once the turn is over. Any row left in the
    // strip missed its terminal update, so clear it rather than leaving a
    // progress bar stuck at the top of the view.
    clearFinishedJobs();
    state.busy = false;
    state.pendingUser = null;
    updateComposerState();
    post({ type: 'refresh_sessions' });
  }
};

function setMode(mode) {
  state.mode = mode;
  $('mode-chat').classList.toggle('active', mode === 'chat');
  $('mode-agent').classList.toggle('active', mode === 'agent');
  if (mode === 'agent' && !state.activeAgent) showAgentDashboard();
  else showChatView();
}

function showChatView() {
  $('chat-view').classList.add('active');
  $('agent-view').classList.remove('active');
  $('back-agents').hidden = !state.activeAgent;
  $('agent-banner').hidden = !state.activeAgent;
  if (state.activeAgent) {
    $('page-title').textContent = state.activeAgent.name;
    $('page-subtitle').textContent = `${friendlyAgentType(state.activeAgent.type)} agent`;
    $('agent-type-badge').textContent = friendlyAgentType(state.activeAgent.type);
    $('agent-banner-name').textContent = state.activeAgent.name;
    $('agent-banner-detail').textContent = state.activeAgent.type === 'webscraper'
      ? (state.activeAgent.site_url || 'Website not configured')
      : state.activeAgent.type === 'task' ? 'Configuration-driven task' : 'Local computer tools enabled';
  } else {
    $('page-title').textContent = 'Chat';
    $('page-subtitle').textContent = 'Private, local conversation';
  }
  renderEmptyState();
  setTimeout(() => $('input').focus(), 0);
}

function showAgentDashboard() {
  state.activeAgent = null;
  syncComposerAgent();
  $('chat-view').classList.remove('active');
  $('agent-view').classList.add('active');
  $('back-agents').hidden = true;
  $('page-title').textContent = 'Agent workspace';
  $('page-subtitle').textContent = 'Reusable local workers and knowledge';
}

function showAgentChat() {
  state.mode = 'agent';
  $('mode-chat').classList.remove('active');
  $('mode-agent').classList.add('active');
  showChatView();
  post({ type: 'select_session', id: state.activeSession });
}

function renderModels() {
  const selects = [$('chat-model'), $('agent-model')];
  for (const select of selects) {
    const previous = select.value;
    select.replaceChildren();
    if (!state.models.length) {
      const option = document.createElement('option');
      option.textContent = 'Add a GGUF model';
      option.value = '';
      select.append(option);
    } else {
      for (const model of state.models) {
        const option = document.createElement('option');
        option.value = model.id;
        option.textContent = model.name;
        option.title = model.path;
        select.append(option);
      }
    }
    select.value = state.activeModel || previous || state.models[0]?.id || '';
  }
}

function syncModelSelections() {
  for (const select of [$('chat-model'), $('agent-model')]) {
    if ([...select.options].some(option => option.value === state.activeModel)) select.value = state.activeModel;
  }
}

function renderSessions() {
  const list = $('session-list');
  list.replaceChildren();
  for (const session of state.sessions) {
    const row = document.createElement('div');
    row.className = `session${session.id === state.activeSession ? ' active' : ''}`;
    row.title = session.title;
    const title = document.createElement('span');
    title.className = 'session-title';
    title.textContent = session.title || 'New conversation';
    const remove = document.createElement('button');
    remove.className = 'session-delete';
    remove.textContent = '×';
    remove.title = 'Delete conversation';
    remove.onclick = event => {
      event.stopPropagation();
      if (state.busy) return toast('Cancel the running turn before deleting conversations.', true);
      post({ type: 'delete_session', id: session.id });
    };
    row.append(title, remove);
    row.onclick = () => {
      state.activeSession = session.id;
      renderSessions();
      post({ type: 'select_session', id: session.id });
    };
    list.append(row);
  }
}

function renderAgents() {
  const grid = $('agent-grid');
  grid.replaceChildren();
  if (!state.agents.length) {
    const empty = document.createElement('div');
    empty.className = 'agent-empty';
    empty.innerHTML = '<div><strong>No agents yet</strong><br><small>Add a local operator, task bot, or site crawler.</small></div>';
    grid.append(empty);
    return;
  }
  for (const agent of state.agents) {
    const card = document.createElement('article');
    card.className = 'agent-card';
    const head = document.createElement('div');
    head.className = 'agent-card-head';
    const info = document.createElement('div');
    const badge = document.createElement('span');
    badge.className = 'badge';
    badge.textContent = friendlyAgentType(agent.type);
    const name = document.createElement('h3');
    name.textContent = agent.name;
    const desc = document.createElement('p');
    desc.textContent = agent.type === 'local'
      ? 'Interactive model with local file and process tools.'
      : agent.type === 'task' ? 'Configuration-driven repeatable workflow.'
      : `Bounded crawler${agent.site_url ? ` for ${agent.site_url}` : ''}.`;
    info.append(badge, name, desc);
    head.append(info);
    const actions = document.createElement('div');
    actions.className = 'agent-card-actions';
    const open = document.createElement('button');
    open.className = 'primary';
    open.textContent = 'Open';
    open.onclick = () => post({ type: 'open_agent', id: agent.id });
    const run = document.createElement('button');
    run.className = 'primary run-agent';
    run.textContent = '▶ Run';
    run.title = 'Open a new session and start the task immediately';
    run.onclick = () => post({ type: 'run_agent', id: agent.id, effort: $('effort')?.value || 'medium' });
    // Perpetual: restart with a fresh context after each batch, forever, until
    // Stop. Distinct from Run so it is never triggered by accident.
    const loop = document.createElement('button');
    loop.className = 'primary run-agent';
    loop.textContent = '\u221E Loop';
    loop.title = 'Run forever: fresh context each batch, dedup on disk. Stop from the top bar.';
    loop.onclick = () => { post({ type: 'run_agent', id: agent.id, perpetual: true, effort: $('effort')?.value || 'high' }); const b=$('stop-agent'); if(b) b.hidden=false; };
    const rename = document.createElement('button');
    rename.className = 'ghost';
    rename.textContent = 'Rename';
    rename.title = 'Rename this agent';
    rename.onclick = () => {
      const next = prompt('Rename agent', agent.name);
      if (next === null) return;             // cancelled
      const trimmed = next.trim();
      if (!trimmed || trimmed === agent.name) return;
      post({ type: 'rename_agent', id: agent.id, name: trimmed });
    };
    const remove = document.createElement('button');
    remove.className = 'ghost';
    remove.textContent = 'Delete';
    remove.onclick = () => {
      if (!confirm(`Delete "${agent.name}"? This cannot be undone.`)) return;
      post({ type: 'delete_agent', id: agent.id });
    };
    actions.append(run, loop, open, rename, remove);
    card.append(head, actions);
    grid.append(card);
  }
}

function renderResources() {
  const rag = state.resources.filter(resource => resource.kind === 'rag');
  const toolpacks = state.resources.filter(resource => resource.kind === 'tool_pack');
  renderResourceList($('rag-list'), rag, '▤');
  renderResourceList($('toolpack-list'), toolpacks, '⌘');
}

function renderResourceList(container, resources, icon) {
  container.replaceChildren();
  if (!resources.length) {
    const empty = document.createElement('div');
    empty.className = 'resource-empty';
    empty.textContent = 'Nothing added yet.';
    container.append(empty);
    return;
  }
  for (const resource of resources) {
    const row = document.createElement('div');
    row.className = 'resource';
    const symbol = document.createElement('span');
    symbol.className = 'resource-icon';
    symbol.textContent = icon;
    const info = document.createElement('div');
    const name = document.createElement('div');
    name.className = 'resource-name';
    name.textContent = resource.name;
    const meta = document.createElement('div');
    meta.className = 'resource-meta';
    meta.textContent = formatBytes(resource.size || 0);
    info.append(name, meta);
    const remove = document.createElement('button');
    remove.className = 'resource-remove';
    remove.textContent = '×';
    remove.title = 'Remove';
    remove.onclick = () => post({ type: 'remove_resource', id: resource.id });
    row.append(symbol, info, remove);
    container.append(row);
  }
}

function renderAgentRagOptions() {
  const box = $('agent-rag-options');
  box.replaceChildren();
  const rag = state.resources.filter(resource => resource.kind === 'rag');
  if (!rag.length) {
    const empty = document.createElement('span');
    empty.className = 'resource-meta';
    empty.textContent = 'No RAG files. Add them from the Agent workspace.';
    box.append(empty);
    return;
  }
  for (const resource of rag) {
    const label = document.createElement('label');
    label.className = 'check-item';
    const input = document.createElement('input');
    input.type = 'checkbox';
    input.value = resource.id;
    const name = document.createElement('span');
    name.textContent = resource.name;
    label.append(input, name);
    box.append(label);
  }
}

function renderAttachments() {
  const strip = $('attachment-strip');
  strip.replaceChildren();
  strip.hidden = !state.attachments.length;
  for (const resource of state.attachments) {
    const chip = document.createElement('div');
    chip.className = 'attachment';
    const name = document.createElement('span');
    name.textContent = resource.name;
    const remove = document.createElement('button');
    remove.textContent = '×';
    remove.onclick = () => {
      state.attachments = state.attachments.filter(item => item.id !== resource.id);
      renderAttachments();
    };
    chip.append(name, remove);
    strip.append(chip);
  }
}

function renderEmptyState() {
  if (transcript.children.length || !transcript.closest('.view.active')) return;
  const empty = document.createElement('div');
  empty.className = 'empty-state';
  empty.id = 'empty-state';
  const icon = document.createElement('img');
  icon.src = 'assets/helm.png';
  icon.alt = '';
  const title = document.createElement('h2');
  title.textContent = state.activeAgent ? state.activeAgent.name : 'What are we building?';
  const text = document.createElement('p');
  text.textContent = state.activeAgent
    ? (state.activeAgent.type === 'webscraper' ? 'Ask this agent to crawl, extract, or organize the configured site.' : 'Give the agent a concrete objective and it can use its enabled local tools.')
    : 'Chat locally with the full tool set, attach working files, or switch to Agent mode to create reusable workers.';
  empty.append(icon, title, text);
  transcript.append(empty);
}

function removeEmptyState() { $('empty-state')?.remove(); }

function addMessage(role, text, pending = false, forceScroll = false) {
  removeEmptyState();
  const item = document.createElement('article');
  item.className = `message ${role}${pending ? ' pending' : ''}`;
  const avatar = document.createElement('div');
  avatar.className = 'avatar';
  avatar.textContent = role === 'user' ? 'N' : 'H';
  const body = document.createElement('div');
  body.className = 'message-body';
  const who = document.createElement('div');
  who.className = 'message-who';
  who.textContent = role === 'user' ? 'You' : (state.activeAgent?.name || 'Helm');
  const content = document.createElement('div');
  content.className = 'message-text';
  body.append(who, content);
  item.append(avatar, body);
  // Raw source is kept on the node so streaming can re-render without having to
  // reverse-engineer text back out of the DOM.
  item._raw = text || '';
  setMessageText(item, item._raw, role);
  transcript.append(item);
  scrollTranscript(forceScroll);
  return item;
}

// User text stays literal — their asterisks and backticks are not formatting.
// Assistant text goes through the renderer.
function setMessageText(item, text, role) {
  const content = item.querySelector('.message-text');
  content.textContent = '';
  if (role === 'user') {
    content.textContent = text;
    content.classList.add('plain');
  } else {
    content.appendChild(window.renderMarkdown(text));
  }
}

// Collapsible reasoning pane, created lazily above the answer.
function thinkingPane(item) {
  let pane = item.querySelector('.thinking');
  if (pane) return pane;
  pane = document.createElement('details');
  pane.className = 'thinking';
  const sum = document.createElement('summary');
  sum.innerHTML = '';
  const label = document.createElement('span');
  label.className = 'thinking-label';
  label.textContent = 'Thinking';
  const meta = document.createElement('span');
  meta.className = 'thinking-meta';
  sum.append(label, meta);
  const bodyEl = document.createElement('div');
  bodyEl.className = 'thinking-body';
  pane.append(sum, bodyEl);
  const body = item.querySelector('.message-body');
  body.insertBefore(pane, body.querySelector('.message-text'));
  return pane;
}

function appendThinking(item, chunk) {
  const pane = thinkingPane(item);
  const bodyEl = pane.querySelector('.thinking-body');
  bodyEl.textContent += chunk;
  const words = bodyEl.textContent.trim().split(/\s+/).filter(Boolean).length;
  pane.querySelector('.thinking-meta').textContent = words ? `${words} words` : '';
}

// ---------------------------------------------------------------- streaming
// Markdown is re-parsed from the raw buffer, which is quadratic if done per
// token. Coalesce to one render per animation frame; assistant_final does a
// last clean pass.
let streamFrame = 0;
function scheduleStreamRender() {
  if (streamFrame) return;
  streamFrame = requestAnimationFrame(() => {
    streamFrame = 0;
    const item = state.streaming;
    if (!item) return;
    setMessageText(item, item._raw || '', 'assistant');
    item.querySelector('.message-text').classList.add('cursor');
    scrollTranscript(false);
  });
}
function cancelStreamRender() {
  if (streamFrame) { cancelAnimationFrame(streamFrame); streamFrame = 0; }
}

function addNote(text) {
  removeEmptyState();
  const el = document.createElement('div');
  el.className = 'agent-note';
  el.textContent = text;
  transcript.append(el);
  scrollTranscript(false);
  return el;
}

// ---------------------------------------------------------------- operators
// Handled before anything reaches the model, so they behave identically in
// chat mode where no tools are registered at all.
const OPERATORS = [
  { name: 'remember', arg: '<fact>',  help: 'Save a fact to long-term memory' },
  { name: 'forget',   arg: '<text>',  help: 'Remove matching memory entries' },
  { name: 'memory',   arg: '',        help: 'Show what is in long-term memory' },
  { name: 'tools',    arg: '',        help: 'List registered tools and which groups are on' },
  { name: 'model',    arg: '<name>',  help: 'Switch model by name' },
  { name: 'new',      arg: '',        help: 'Start a new conversation' },
  { name: 'help',     arg: '',        help: 'Show this list' }
];

function runOperator(raw) {
  const m = raw.match(/^\/([a-zA-Z_]+)\s*([\s\S]*)$/);
  if (!m) return false;
  const name = m[1].toLowerCase();
  const args = m[2].trim();
  const known = OPERATORS.some(op => op.name === name);
  if (!known) { toast(`Unknown command: /${name}. Try /help.`, true); return true; }

  if (name === 'help') {
    addToolChip('/help', OPERATORS.map(op => `/${op.name} ${op.arg}`.trim() + ' — ' + op.help).join('\n'), 'result');
    return true;
  }
  if (name === 'new') { post({ type: 'new_session' }); return true; }
  if (name === 'model') {
    if (!args) { toast('Usage: /model <name>', true); return true; }
    const want = args.toLowerCase();
    const hit = (state.models || []).find(mo => (mo.name || '').toLowerCase().includes(want));
    if (!hit) { toast(`No model matching "${args}".`, true); return true; }
    post({ type: 'load_model', id: hit.id });
    return true;
  }
  if (name === 'memory') { openSettings('memory'); }
  post({ type: 'slash_command', name, args, session_id: state.activeSession || '' });
  return true;
}

// ---------------------------------------------------------------- slash menu
function updateSlashMenu() {
  const menu = $('slash-menu');
  if (!menu) return;
  const value = $('input').value;
  const m = value.match(/^\/([a-zA-Z_]*)$/);
  if (!m) { menu.hidden = true; return; }
  const prefix = m[1].toLowerCase();
  const hits = OPERATORS.filter(op => op.name.startsWith(prefix));
  if (!hits.length) { menu.hidden = true; return; }
  menu.textContent = '';
  hits.forEach((op, index) => {
    const row = document.createElement('button');
    row.type = 'button';
    row.className = 'slash-item' + (index === 0 ? ' active' : '');
    const cmd = document.createElement('span');
    cmd.className = 'slash-cmd';
    cmd.textContent = `/${op.name} ${op.arg}`.trim();
    const help = document.createElement('span');
    help.className = 'slash-help';
    help.textContent = op.help;
    row.append(cmd, help);
    row.addEventListener('click', () => {
      $('input').value = `/${op.name} `;
      menu.hidden = true;
      $('input').focus();
      resizeInput();
    });
    menu.appendChild(row);
  });
  menu.hidden = false;
}

function addToolChip(label, text, kind) {
  removeEmptyState();
  const chip = document.createElement('div');
  chip.className = `tool-chip ${kind || ''}`;
  const head = document.createElement('span');
  head.className = 'tool-label';
  head.textContent = label;
  chip.append(head, document.createTextNode(text));
  transcript.append(chip);
  scrollTranscript(false);
  return chip;
}

// The composer carries the active agent's name so it is never ambiguous which
// instrument the next message drives.
function syncComposerAgent() {
  const wrap = $('composer-wrap');
  const input = $('input');
  if (!wrap) return;
  const agent = state.activeAgent;
  if (agent) {
    wrap.classList.add('agent-active');
    const composer = wrap.querySelector('.composer');
    if (composer) composer.dataset.agentLabel = `${agent.type || 'agent'} · ${agent.name}`;
    if (input) input.placeholder = `Direct ${agent.name}`;
  } else {
    wrap.classList.remove('agent-active');
    if (input) input.placeholder = 'Message Helm';
  }
}

function clearFinishedJobs() {
  const strip = $('jobs');
  if (!strip) return;
  for (const row of [...strip.children]) row.remove();
}

function renderJob(message) {
  let row = $(`job-${message.id}`);
  if (!row) {
    row = document.createElement('div');
    row.id = `job-${message.id}`;
    row.className = 'job';
    const name = document.createElement('span'); name.className = 'job-name';
    const track = document.createElement('div'); track.className = 'job-track'; track.append(document.createElement('i'));
    const note = document.createElement('span'); note.className = 'job-note';
    const cancel = document.createElement('button'); cancel.textContent = 'Cancel'; cancel.onclick = () => post({ type: 'job_cancel', id: message.id });
    row.append(name, track, note, cancel);
    $('jobs').append(row);
  }
  row.querySelector('.job-name').textContent = `${message.name} #${message.id}`;
  row.querySelector('.job-track i').style.width = `${Math.max(0, Math.min(100, message.progress || 0))}%`;
  const note = String(message.note || '');
  row.querySelector('.job-note').textContent = note.length > 160 ? note.slice(0, 160) + '…' : note;
  if (message.status && message.status !== 'running') {
    row.querySelector('button')?.remove();
    setTimeout(() => row.remove(), 8000);
  }
}

function sendMessage() {
  const text = $('input').value.trim();
  if (!text) return;
  // Operators run without the model, so they work while busy and with nothing
  // loaded — that is the point of intercepting rather than prompting.
  if (text.startsWith('/')) {
    const menu = $('slash-menu');
    if (menu) menu.hidden = true;
    if (runOperator(text)) {
      $('input').value = '';
      resizeInput();
      return;
    }
  }
  if (state.busy || !state.activeSession) return;
  if (!state.modelLoaded) {
    if (state.activeModel) post({ type: 'load_model', id: state.activeModel });
    return toast('Load a model before sending.', true);
  }
  const attached = [...state.attachments];
  state.followOutput = true;
  state.pendingUser = addMessage('user', text, true, true);
  if (attached.length) {
    const names = attached.map(item => item.name).join(', ');
    const note = document.createElement('div');
    note.className = 'resource-meta';
    note.textContent = `Attached: ${names}`;
    state.pendingUser.querySelector('.message-body').append(note);
  }
  state.busy = true;
  updateComposerState();
  $('input').value = '';
  resizeInput();
  post({
    type: 'send',
    session_id: state.activeSession,
    text,
    mode: state.activeAgent ? 'agent' : 'chat',
    agent_id: state.activeAgent?.id || '',
    effort: $('effort').value,
    resource_ids: attached.map(item => item.id)
  });
}

function updateComposerState() {
  $('send').disabled = state.busy;
  $('stop').hidden = !state.busy;
  $('send').hidden = state.busy;
  $('input').disabled = false;
  $('chat-model').disabled = state.busy;
  $('effort').disabled = state.busy;
  $('add-model').disabled = state.busy;
  $('attach-files').disabled = state.busy;
  $('mic').disabled = state.transcribing;
  updateMicState();
}

function resizeInput() {
  const input = $('input');
  input.style.height = 'auto';
  input.style.height = `${Math.min(input.scrollHeight, 210)}px`;
}

function openTaskModal() {
  state.selectedAgentType = 'local';
  state.pendingConfig = null;
  $('agent-config-name').textContent = 'No file selected';
  $('site-url').value = '';
  $('agent-name').value = 'Local operator';
  for (const card of document.querySelectorAll('.type-card')) card.classList.toggle('selected', card.dataset.agentType === 'local');
  updateTaskTypeFields();
  renderAgentRagOptions();
  renderPermOptions(PERM_PRESETS.recommended);
  for (const b of document.querySelectorAll('.perm-preset')) b.classList.toggle('selected', b.dataset.perm === 'recommended');
  $('task-modal').hidden = false;
}

function closeTaskModal() { $('task-modal').hidden = true; }

function updateTaskTypeFields() {
  $('agent-config-row').hidden = state.selectedAgentType !== 'task';
  $('site-url-row').hidden = state.selectedAgentType !== 'webscraper';
  if (state.selectedAgentType === 'local') $('agent-name').value = 'Local operator';
  if (state.selectedAgentType === 'task') $('agent-name').value = 'Task bot';
  if (state.selectedAgentType === 'webscraper') $('agent-name').value = 'Site crawler';
}


// Permission groups resolve to the EXACT tool names the registry registers.
// If a tool is renamed in C++, its entry here must change too, or the group
// silently grants nothing. task_complete is intentionally absent: the loop
// always injects it so an autonomous run keeps a reachable exit.
const PERM_GROUPS = [
  { id: 'web',      label: 'Web & GitHub search',   tools: ['search_web','fetch_web_page','crawl_site','github_search'] },
  { id: 'archive',  label: 'Offline archive search', tools: ['search_archive'] },
  { id: 'loopstate',label: 'Loop state (perpetual)', tools: ['archive_seen'] },
  { id: 'fileread', label: 'Read files',            tools: ['read_text_file','list_directory'] },
  { id: 'filewrite',label: 'Write files',           tools: ['write_text_file'] },
  { id: 'docs',     label: 'Read documents',        tools: ['extract_document'] },
  { id: 'memread',  label: 'Read memory',           tools: ['recall_memory'] },
  { id: 'memwrite', label: 'Write memory',          tools: ['remember','forget'] },
  { id: 'images',   label: 'Generate images',       tools: ['generate_image'] },
  { id: 'voice',    label: 'Speak',                 tools: ['speak_text'] },
  { id: 'deskview', label: 'Desktop screenshot',    tools: ['desktop_screenshot'] },
  { id: 'deskctl',  label: 'Desktop control',       tools: ['desktop_click','desktop_type','desktop_hotkey'] },
  { id: 'process',  label: 'Run processes',         tools: ['run_process'] },
  { id: 'utils',    label: 'Utilities',             tools: ['get_time','roll_dice'] },
];
const PERM_PRESETS = {
  recommended: ['web','archive','loopstate','fileread','filewrite','docs','memread','memwrite','utils'],
  readonly:    ['web','archive','fileread','docs','memread','utils'],
  full:        PERM_GROUPS.map(g => g.id),
};
function renderPermOptions(presetGroups) {
  const box = $('agent-perm-options');
  if (!box) return;
  const on = new Set(presetGroups);
  box.innerHTML = '';
  for (const g of PERM_GROUPS) {
    const id = 'perm-' + g.id;
    const label = document.createElement('label');
    const cb = document.createElement('input');
    cb.type = 'checkbox'; cb.value = g.id; cb.id = id; cb.checked = on.has(g.id);
    const span = document.createElement('span'); span.textContent = g.label;
    label.append(cb, span); box.append(label);
  }
}
function selectedAllowedTools() {
  const box = $('agent-perm-options');
  const chosen = new Set([...box.querySelectorAll('input:checked')].map(i => i.value));
  const tools = [];
  for (const g of PERM_GROUPS) if (chosen.has(g.id)) tools.push(...g.tools);
  return tools;
}

function createAgent() {
  const name = $('agent-name').value.trim() || 'Agent';
  const modelId = $('agent-model').value || state.activeModel;
  const siteUrl = $('site-url').value.trim();
  if (!modelId) return toast('Add a GGUF model before creating an agent.', true);
  if (state.selectedAgentType === 'task' && !state.pendingConfig) return toast('Choose a task configuration file.', true);
  if (state.selectedAgentType === 'webscraper' && !/^https?:\/\//i.test(siteUrl)) return toast('Enter a valid http or https URL.', true);
  const ragIds = [...$('agent-rag-options').querySelectorAll('input:checked')].map(input => input.value);
  post({
    type: 'create_agent',
    agent: {
      name,
      type: state.selectedAgentType,
      model_id: modelId,
      config_resource_id: state.pendingConfig?.id || '',
      site_url: siteUrl,
      rag_ids: ragIds,
      allowed_tools: selectedAllowedTools(),
      permissions_configured: true
    }
  });
}

function toast(text, isError = false) {
  const item = document.createElement('div');
  item.className = `toast${isError ? ' error' : ''}`;
  item.textContent = text;
  $('toast-region').append(item);
  setTimeout(() => item.remove(), 4200);
}

function friendlyAgentType(type) {
  return type === 'webscraper' ? 'Web scraper' : type === 'task' ? 'Task bot' : 'Local';
}
function formatBytes(value) {
  if (!value) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB'];
  const index = Math.min(units.length - 1, Math.floor(Math.log(value) / Math.log(1024)));
  return `${(value / Math.pow(1024, index)).toFixed(index ? 1 : 0)} ${units[index]}`;
}

function isNearTranscriptBottom() {
  return transcript.scrollHeight - transcript.scrollTop - transcript.clientHeight < 72;
}

function scrollTranscript(force = false) {
  if (!force && !state.followOutput) {
    $('jump-bottom').hidden = false;
    return;
  }
  requestAnimationFrame(() => {
    transcript.scrollTop = transcript.scrollHeight;
    state.followOutput = true;
    $('jump-bottom').hidden = true;
  });
}

function renderBuiltInTools() {
  const list = $('builtin-tool-list');
  if (!list) return;
  list.replaceChildren();
  if (!state.tools.length) {
    const empty = document.createElement('div');
    empty.className = 'resource-empty';
    empty.textContent = 'Tool registry has not loaded yet.';
    list.append(empty);
    return;
  }
  const detected = state.settingsDetected || {};
  const readiness = name => {
    if (name === 'search_archive') return state.settings.enable_archive_tools !== false && !!detected.python && !!detected.archive;
    if (name === 'search_web' || name === 'fetch_web_page') return state.settings.enable_web_tools !== false && !!detected.python;
    if (name === 'generate_image') return state.settings.enable_image_tools !== false && !!detected.python && !!detected.comfy_workflow;
    if (name === 'speak_text') return state.settings.enable_voice_tools !== false && !!detected.piper && !!detected.piper_voice;
    if (name === 'describe_image') return state.settings.enable_vision_tools === true && !!detected.vision_cli && !!detected.vision_model && !!detected.vision_mmproj;
    if (name === 'extract_document') return state.settings.enable_document_tools !== false && !!detected.python;
    if (name.startsWith('desktop_')) return state.settings.enable_desktop_tools === true && !!detected.python;
    return true;
  };
  const external = new Set(['search_web', 'fetch_web_page', 'search_archive', 'generate_image', 'speak_text', 'extract_document', 'desktop_screenshot', 'desktop_click', 'desktop_type', 'desktop_hotkey']);
  for (const tool of state.tools) {
    const row = document.createElement('div');
    row.className = 'tooling-row';
    const info = document.createElement('div');
    const name = document.createElement('strong');
    name.textContent = tool.name;
    const desc = document.createElement('span');
    desc.textContent = tool.description || (tool.job ? 'Background job tool' : 'Local tool');
    info.append(name, desc);
    const status = document.createElement('span');
    const ready = readiness(tool.name);
    status.className = `tool-status ${ready ? 'ready' : 'missing'}`;
    status.textContent = external.has(tool.name) ? (ready ? 'Ready' : 'Setup needed') : (tool.job ? 'Job' : 'Built in');
    row.append(info, status);
    list.append(row);
  }
}

function openSettings(section) {
  $('settings-modal').hidden = false;
  post({ type: 'get_settings' });
  post({ type: 'get_memory' });
  post({ type: 'hardware_info' });
  renderModelManageList();
  if (section === 'memory') {
    setTimeout(() => {
      const el = $('memory-section');
      if (el) el.scrollIntoView({ behavior: 'smooth', block: 'start' });
    }, 0);
  }
}

function closeSettings() { $('settings-modal').hidden = true; }

function populateSettings() {
  const v = state.settings || {};
  const setValue = (id, value) => { if ($(id) && value !== undefined && value !== null) $(id).value = value; };
  setValue('setting-n-ctx', v.n_ctx);
  setValue('setting-gpu-layers', v.n_gpu_layers);
  setValue('setting-batch', v.n_batch);
  setValue('setting-ubatch', v.n_ubatch);
  setValue('setting-threads', v.n_threads);
  setValue('setting-threads-batch', v.n_threads_batch);
  setValue('setting-flash', v.flash_attention || 'auto');
  setValue('setting-kv-location', v.kv_cache_location || 'vram');
  setValue('setting-kv-type', v.kv_cache_type || 'f16');
  setValue('setting-temperature', v.temperature ?? 0.7);
  setValue('setting-top-p', v.top_p ?? 1.0);
  setValue('setting-top-k', v.top_k ?? 40);
  setValue('setting-min-p', v.min_p ?? 0.05);
  setValue('setting-repeat-penalty', v.repeat_penalty ?? 1.0);
  setValue('setting-repeat-last-n', v.repeat_last_n ?? 64);
  setValue('setting-tool-root', v.tool_root || 'F:\\AI Tools');
  setValue('setting-comfy-url', v.comfyui_url || 'http://127.0.0.1:8188');
  setValue('setting-comfy-workflow', v.comfyui_workflow || '');
  $('setting-web-tools').checked = v.enable_web_tools !== false;
  $('setting-image-tools').checked = v.enable_image_tools !== false;
  $('setting-voice-tools').checked = v.enable_voice_tools !== false;
  $('setting-document-tools').checked = v.enable_document_tools !== false;
  $('setting-desktop-tools').checked = v.enable_desktop_tools === true;
  $('setting-compression').checked = v.enable_compression !== false;
  $('setting-archive-tools').checked = v.enable_archive_tools !== false;
  $('setting-vision-tools').checked = v.enable_vision_tools === true;
  setValue('setting-vision-model', v.vision_model || '');
  setValue('setting-vision-mmproj', v.vision_mmproj || '');
  setValue('setting-vision-cli', v.vision_cli_exe || '');
  setValue('setting-write-root', v.write_root || '');
  setValue('setting-image-output-dir', v.image_output_dir || '');
  setValue('setting-archive-db', v.archive_db || '');
  setValue('setting-archive-shards', v.archive_shards || '');
  renderDetection();
}

function renderDetection() {
  const box = $('tool-detection');
  box.replaceChildren();
  const names = {
    python: 'Tool runtime', ffmpeg: 'FFmpeg', whisper: 'Whisper', whisper_model: 'Whisper model',
    piper: 'Piper', piper_voice: 'Piper voice', comfy_workflow: 'Comfy workflow',
    browser: 'JS page rendering', archive: 'Offline archive',
    vision_cli: 'Vision CLI', vision_model: 'Vision model', vision_mmproj: 'Vision mmproj'
  };
  for (const [key, label] of Object.entries(names)) {
    const chip = document.createElement('span');
    const ready = !!state.settingsDetected?.[key];
    chip.className = `detection ${ready ? 'ready' : 'missing'}`;
    chip.textContent = `${ready ? '✓' : '×'} ${label}`;
    box.append(chip);
  }
}

// Presets scaled to the detected machine, instead of hardcoded for one 4090 /
// 96GB rig. Deriving an absolute "context that fits" number would require
// knowing the loaded model's weight size, which is not known at Settings-open
// time (no model may be selected yet) - guessing that produces a context that
// looks fine here and OOMs the instant real weights load alongside it.
// Instead, scale the THREE VALUES THAT ARE KNOWN TO WORK on the reference
// 4090 (24GB) / 96GB machine by the ratio of the user's actual hardware to
// that reference, clamped to sane floors and ceilings. Less precise, but it
// cannot recommend a number that ignores weight size, and it degrades toward
// the reference numbers (not zero) when detection fails.
function computePresets() {
  const refVramMb = 24576;   // reference: RTX 4090
  const refRamMb = 98304;    // reference: 96GB system
  const vramMb = state.hardware?.vram_mb || refVramMb;
  const ramMb = state.hardware?.ram_mb || refRamMb;
  const vramRatio = Math.max(0.15, Math.min(2.5, vramMb / refVramMb));
  const ramRatio = Math.max(0.15, Math.min(2.5, ramMb / refRamMb));
  const roundKto = (n, step) => Math.max(step, Math.round(n / step) * step);
  const fastCtx     = roundKto(8192  * vramRatio, 1024);
  const balancedCtx = roundKto(32768 * vramRatio, 2048);
  const hybridCtx   = roundKto(65536 * ramRatio,  4096);
  // Sampling defaults tuned for coder-class MoE/dense models: mild repeat
  // penalty (1.05) is cheap insurance against the newline-collapse failure
  // without hurting normal generation, and matches the values known to work
  // well for this model class.
  const sampling = { temperature: 0.6, top_p: 0.95, top_k: 20, min_p: 0.05, repeat_penalty: 1.05, repeat_last_n: 64 };
  return {
    fast:     { n_ctx: fastCtx,     n_gpu_layers: 999, n_batch: 1024, n_ubatch: 512, n_threads: 0, n_threads_batch: 0, flash: 'on', kv_location: 'vram', kv_type: 'f16',  ...sampling },
    balanced: { n_ctx: balancedCtx, n_gpu_layers: 999, n_batch: 1024, n_ubatch: 512, n_threads: 0, n_threads_batch: 0, flash: 'on', kv_location: 'vram', kv_type: 'q8_0', ...sampling },
    hybrid:   { n_ctx: hybridCtx,   n_gpu_layers: 999, n_batch: 512,  n_ubatch: 256, n_threads: 0, n_threads_batch: 0, flash: 'on', kv_location: 'ram',  kv_type: 'q8_0', ...sampling },
  };
}


// "Installed models" pane in Settings. Solves the case where a model file was
// deleted by hand outside Helm (Explorer, disk cleanup) and now sits as a
// dead entry pointing at a missing path - remove clears the catalog entry.
// The optional checkbox also deletes the file itself, so removing a model
// Helm still has on disk is a single action instead of hunting the path down
// in Explorer separately.
function renderModelManageList() {
  const box = $('model-manage-list');
  if (!box) return;
  box.replaceChildren();
  if (!state.models.length) {
    const empty = document.createElement('p');
    empty.className = 'perm-hint';
    empty.textContent = 'No models added yet. Use "Add model" above to browse for a GGUF file.';
    box.append(empty);
    return;
  }
  for (const m of state.models) {
    const row = document.createElement('div');
    row.className = 'model-manage-row';
    const info = document.createElement('div');
    info.className = 'model-manage-info';
    const name = document.createElement('strong');
    name.textContent = m.name || m.id;
    const path = document.createElement('small');
    path.textContent = m.path || '';
    info.append(name, path);
    const controls = document.createElement('div');
    controls.className = 'model-manage-controls';
    const delFileLabel = document.createElement('label');
    delFileLabel.className = 'model-manage-delfile';
    const delFileCb = document.createElement('input');
    delFileCb.type = 'checkbox';
    delFileLabel.append(delFileCb, document.createTextNode(' also delete file'));
    const remove = document.createElement('button');
    remove.className = 'ghost';
    remove.textContent = 'Remove';
    remove.onclick = () => {
      const deleteFile = delFileCb.checked;
      const msg = deleteFile
        ? `Remove "${m.name}" and permanently delete the file from disk?`
        : `Remove "${m.name}" from the list? The file stays on disk.`;
      if (!confirm(msg)) return;
      post({ type: 'remove_model', id: m.id, delete_file: deleteFile });
    };
    controls.append(delFileLabel, remove);
    row.append(info, controls);
    box.append(row);
  }
}

function applyPreset(name) {
  const v = computePresets()[name];
  if (!v) return;
  $('setting-n-ctx').value = v.n_ctx;
  $('setting-gpu-layers').value = v.n_gpu_layers;
  $('setting-batch').value = v.n_batch;
  $('setting-ubatch').value = v.n_ubatch;
  $('setting-threads').value = v.n_threads;
  $('setting-threads-batch').value = v.n_threads_batch;
  $('setting-flash').value = v.flash;
  $('setting-kv-location').value = v.kv_location;
  $('setting-kv-type').value = v.kv_type;
  if (v.temperature !== undefined) $('setting-temperature').value = v.temperature;
  if (v.top_p !== undefined) $('setting-top-p').value = v.top_p;
  if (v.top_k !== undefined) $('setting-top-k').value = v.top_k;
  if (v.min_p !== undefined) $('setting-min-p').value = v.min_p;
  if (v.repeat_penalty !== undefined) $('setting-repeat-penalty').value = v.repeat_penalty;
  if (v.repeat_last_n !== undefined) $('setting-repeat-last-n').value = v.repeat_last_n;
}

function saveSettings() {
  const integer = (id, fallback) => {
    const value = Number.parseInt($(id).value, 10);
    return Number.isFinite(value) ? value : fallback;
  };
  const decimal = (id, fallback) => {
    const value = Number.parseFloat($(id).value);
    return Number.isFinite(value) ? value : fallback;
  };
  post({
    type: 'save_settings',
    values: {
      n_ctx: integer('setting-n-ctx', 8192),
      n_gpu_layers: integer('setting-gpu-layers', 999),
      n_batch: integer('setting-batch', 512),
      n_ubatch: integer('setting-ubatch', 256),
      n_threads: integer('setting-threads', 0),
      n_threads_batch: integer('setting-threads-batch', 0),
      flash_attention: $('setting-flash').value,
      kv_cache_location: $('setting-kv-location').value,
      kv_cache_type: $('setting-kv-type').value,
      temperature: decimal('setting-temperature', 0.7),
      top_p: decimal('setting-top-p', 1.0),
      top_k: integer('setting-top-k', 40),
      min_p: decimal('setting-min-p', 0.05),
      repeat_penalty: decimal('setting-repeat-penalty', 1.0),
      repeat_last_n: integer('setting-repeat-last-n', 64),
      tool_root: $('setting-tool-root').value.trim(),
      comfyui_url: $('setting-comfy-url').value.trim(),
      comfyui_workflow: $('setting-comfy-workflow').value.trim(),
      enable_web_tools: $('setting-web-tools').checked,
      enable_image_tools: $('setting-image-tools').checked,
      enable_voice_tools: $('setting-voice-tools').checked,
      enable_document_tools: $('setting-document-tools').checked,
      enable_desktop_tools: $('setting-desktop-tools').checked,
      enable_compression: $('setting-compression').checked,
      enable_archive_tools: $('setting-archive-tools').checked,
      enable_vision_tools: $('setting-vision-tools').checked,
      vision_model: $('setting-vision-model').value.trim(),
      vision_mmproj: $('setting-vision-mmproj').value.trim(),
      vision_cli_exe: $('setting-vision-cli').value.trim(),
      write_root: $('setting-write-root').value.trim(),
      image_output_dir: $('setting-image-output-dir').value.trim(),
      archive_db: $('setting-archive-db').value.trim(),
      archive_shards: $('setting-archive-shards').value.trim()
    }
  });
}

async function toggleMicrophone() {
  if (state.transcribing) return;
  if (state.recording) {
    state.mediaRecorder?.stop();
    return;
  }
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true }, video: false });
    const preferred = ['audio/webm;codecs=opus', 'audio/webm', 'audio/ogg;codecs=opus'];
    const mimeType = preferred.find(type => window.MediaRecorder?.isTypeSupported?.(type)) || '';
    state.mediaStream = stream;
    state.audioChunks = [];
    state.mediaRecorder = mimeType ? new MediaRecorder(stream, { mimeType }) : new MediaRecorder(stream);
    state.mediaRecorder.ondataavailable = event => { if (event.data?.size) state.audioChunks.push(event.data); };
    state.mediaRecorder.onerror = () => {
      stopMediaStream();
      state.recording = false;
      updateMicState();
      toast('Microphone recording failed.', true);
    };
    state.mediaRecorder.onstop = async () => {
      const type = state.mediaRecorder?.mimeType || mimeType || 'audio/webm';
      const blob = new Blob(state.audioChunks, { type });
      stopMediaStream();
      state.recording = false;
      if (!blob.size) { updateMicState(); return; }
      state.transcribing = true;
      updateMicState('Uploading audio…');
      try {
        post({ type: 'transcribe_audio', mime: type, data: await blobToBase64(blob) });
      } catch (error) {
        state.transcribing = false;
        updateMicState();
        toast(`Could not prepare recording: ${error.message || error}`, true);
      }
    };
    state.mediaRecorder.start(250);
    state.recording = true;
    updateMicState();
  } catch (error) {
    toast(`Microphone unavailable: ${error.message || error}`, true);
  }
}

function stopMediaStream() {
  for (const track of state.mediaStream?.getTracks?.() || []) track.stop();
  state.mediaStream = null;
}

async function blobToBase64(blob) {
  const bytes = new Uint8Array(await blob.arrayBuffer());
  let binary = '';
  const chunk = 0x8000;
  for (let offset = 0; offset < bytes.length; offset += chunk) {
    binary += String.fromCharCode(...bytes.subarray(offset, offset + chunk));
  }
  return btoa(binary);
}

function insertTranscriptText(text) {
  if (!text) return toast('Whisper returned no speech.', true);
  const input = $('input');
  const start = input.selectionStart ?? input.value.length;
  const end = input.selectionEnd ?? input.value.length;
  const prefix = input.value.slice(0, start);
  const suffix = input.value.slice(end);
  const separator = prefix && !/\s$/.test(prefix) ? ' ' : '';
  input.value = `${prefix}${separator}${text}${suffix}`;
  const cursor = prefix.length + separator.length + text.length;
  input.setSelectionRange(cursor, cursor);
  resizeInput();
  input.focus();
}

function updateMicState(note = '') {
  const button = $('mic');
  button.classList.toggle('recording', state.recording);
  button.classList.toggle('transcribing', state.transcribing);
  button.disabled = state.transcribing;
  button.innerHTML = state.recording ? '<span>■</span> Stop' : state.transcribing ? '<span>◌</span> Whisper' : '<span>●</span> Mic';
  button.title = state.recording ? 'Stop recording' : state.transcribing ? (note || 'Whisper is transcribing') : 'Record with Whisper';
  if (state.recording) $('composer-hint').textContent = 'Recording… click Stop when finished';
  else if (state.transcribing) $('composer-hint').textContent = note || 'Whisper is transcribing…';
  else $('composer-hint').textContent = 'Enter to send · Shift+Enter for newline';
}

const _stopBtn = $('stop-agent'); if (_stopBtn) _stopBtn.onclick = () => { post({ type: 'stop_agent' }); _stopBtn.hidden = true; };
$('mode-chat').onclick = () => { state.activeAgent = null; syncComposerAgent(); setMode('chat'); };
$('mode-agent').onclick = () => { state.activeAgent = null; syncComposerAgent(); setMode('agent'); };
$('back-agents').onclick = () => { state.activeAgent = null; showAgentDashboard(); };
$('new-chat').onclick = () => {
  state.activeAgent = null;
  setMode('chat');
  post({ type: 'new_session' });
};
$('refresh-history').onclick = () => post({ type: 'refresh_sessions' });
$('send').onclick = sendMessage;
$('stop').onclick = () => post({ type: 'cancel' });
$('attach-files').onclick = () => post({ type: 'pick_attachments' });
$('mic').onclick = toggleMicrophone;
$('jump-bottom').onclick = () => { state.followOutput = true; scrollTranscript(true); };
$('add-model').onclick = () => post({ type: 'add_model' });
$('unload-model').onclick = () => post({ type: 'unload_model' });
$('open-settings').onclick = openSettings;
$('close-settings').onclick = closeSettings;
$('cancel-settings').onclick = closeSettings;
$('save-settings').onclick = saveSettings;
$('choose-comfy-workflow').onclick = () => post({ type: 'pick_comfy_workflow' });
$('open-tool-root').onclick = () => post({ type: 'open_tool_root' });
for (const button of document.querySelectorAll('.preset')) button.onclick = () => applyPreset(button.dataset.preset);
$('chat-model').onchange = event => {
  if (!event.target.value || event.target.value === state.activeModel) return;
  post({ type: 'load_model', id: event.target.value });
};
$('input').addEventListener('input', () => { resizeInput(); updateSlashMenu(); });
$('input').addEventListener('keydown', event => {
  const menu = $('slash-menu');
  if (menu && !menu.hidden) {
    if (event.key === 'Escape') { menu.hidden = true; return; }
    if (event.key === 'Tab') {
      const first = menu.querySelector('.slash-item .slash-cmd');
      if (first) {
        event.preventDefault();
        $('input').value = first.textContent.split(' ')[0] + ' ';
        menu.hidden = true;
        resizeInput();
        return;
      }
    }
  }
  if (event.key === 'Enter' && !event.shiftKey) {
    event.preventDefault();
    sendMessage();
  }
});
$('input').addEventListener('blur', () => {
  setTimeout(() => { const menu = $('slash-menu'); if (menu) menu.hidden = true; }, 150);
});

// Memory editor
if ($('save-memory')) {
  $('save-memory').onclick = () => post({ type: 'save_memory', text: $('memory-text').value });
}
if ($('memory-text')) {
  $('memory-text').addEventListener('input', () => {
    const meter = $('memory-meter');
    if (!meter || !state.memory) return;
    const bytes = new TextEncoder().encode($('memory-text').value).length;
    meter.textContent = `${bytes} / ${state.memory.budget} bytes`;
    meter.classList.toggle('over', bytes > state.memory.budget);
  });
}

$('add-agent').onclick = openTaskModal;
for (const btn of document.querySelectorAll('.perm-preset')) {
  btn.onclick = () => {
    renderPermOptions(PERM_PRESETS[btn.dataset.perm] || PERM_PRESETS.recommended);
    for (const b of document.querySelectorAll('.perm-preset')) b.classList.toggle('selected', b === btn);
  };
}
$('close-task').onclick = closeTaskModal;
$('cancel-task').onclick = closeTaskModal;
$('create-agent').onclick = createAgent;
$('choose-agent-config').onclick = () => post({ type: 'import_agent_config' });
$('add-rag').onclick = () => post({ type: 'import_rag' });
$('add-toolpack').onclick = () => post({ type: 'import_tool_pack' });
$('open-rag').onclick = () => { state.activeAgent = null; setMode('agent'); setTimeout(() => $('rag-panel').scrollIntoView({ behavior: 'smooth' }), 0); };
$('open-tools').onclick = () => { state.activeAgent = null; setMode('agent'); setTimeout(() => $('tools-panel').scrollIntoView({ behavior: 'smooth' }), 0); };
$('task-modal').addEventListener('click', event => { if (event.target === $('task-modal')) closeTaskModal(); });
$('settings-modal').addEventListener('click', event => { if (event.target === $('settings-modal')) closeSettings(); });
transcript.addEventListener('scroll', () => {
  state.followOutput = isNearTranscriptBottom();
  $('jump-bottom').hidden = state.followOutput;
}, { passive: true });
for (const card of document.querySelectorAll('.type-card')) {
  card.onclick = () => {
    state.selectedAgentType = card.dataset.agentType;
    for (const item of document.querySelectorAll('.type-card')) item.classList.toggle('selected', item === card);
    updateTaskTypeFields();
  };
}

document.addEventListener('keydown', event => {
  if (event.key !== 'Escape') return;
  if (!$('task-modal').hidden) closeTaskModal();
  if (!$('settings-modal').hidden) closeSettings();
  if (state.recording) state.mediaRecorder?.stop();
});

renderEmptyState();
updateComposerState();
post({ type: 'ui_ready' });

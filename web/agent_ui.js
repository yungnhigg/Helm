/* Agent cards and permission selection are isolated from app.js so the bridge
   script remains readable as the workspace grows. No framework/build step. */
'use strict';

window.HelmAgentUI = (() => {
  const permissionGroups = [
    { id: 'web',       label: 'Web & GitHub search',    tools: ['search_web','fetch_web_page','crawl_site','github_search'] },
    { id: 'archive',   label: 'Offline archive search', tools: ['search_archive'] },
    { id: 'loopstate', label: 'Loop state (perpetual)', tools: ['archive_seen'] },
    { id: 'fileread',  label: 'Read files',             tools: ['read_text_file','list_directory'] },
    { id: 'filewrite', label: 'Write files',            tools: ['write_text_file'] },
    { id: 'docs',      label: 'Read documents',         tools: ['extract_document'] },
    { id: 'memread',   label: 'Read memory',            tools: ['recall_memory'] },
    { id: 'memwrite',  label: 'Write memory',           tools: ['remember','forget'] },
    { id: 'images',    label: 'Generate images',        tools: ['generate_image'] },
    { id: 'voice',     label: 'Speak',                  tools: ['speak_text'] },
    { id: 'deskview',  label: 'Desktop screenshot',     tools: ['desktop_screenshot'] },
    { id: 'deskctl',   label: 'Desktop control',        tools: ['desktop_click','desktop_type','desktop_hotkey'] },
    { id: 'process',   label: 'Run processes',          tools: ['run_process'] },
    { id: 'utils',     label: 'Utilities',              tools: ['get_time','roll_dice'] }
  ];
  const permissionPresets = Object.freeze({
    recommended: ['web','archive','loopstate','fileread','filewrite','docs','memread','memwrite','utils'],
    readonly: ['web','archive','fileread','docs','memread','utils'],
    full: permissionGroups.map(group => group.id)
  });

  function friendlyType(type) {
    return type === 'webscraper' ? 'Web scraper' : type === 'task' ? 'Task bot' : 'Local';
  }

  function actionButton({ className, icon, label, title, click }) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = className;
    button.title = title || '';
    if (icon) {
      const glyph = document.createElement('span');
      glyph.className = 'agent-action-icon';
      glyph.textContent = icon;
      button.append(glyph);
    }
    const text = document.createElement('span');
    text.textContent = label;
    button.append(text);
    button.onclick = click;
    return button;
  }

  function renderAgents({ grid, agents, post, effort, revealStop, toast }) {
    grid.replaceChildren();
    if (!agents.length) {
      const empty = document.createElement('div');
      empty.className = 'agent-empty';
      const content = document.createElement('div');
      const title = document.createElement('strong');
      title.textContent = 'No agents yet';
      const line = document.createElement('br');
      const detail = document.createElement('small');
      detail.textContent = 'Add a local operator, task bot, or site crawler.';
      content.append(title, line, detail);
      empty.append(content);
      grid.append(empty);
      return;
    }

    for (const agent of agents) {
      const card = document.createElement('article');
      card.className = 'agent-card';
      const head = document.createElement('div');
      head.className = 'agent-card-head';
      const info = document.createElement('div');
      const badge = document.createElement('span');
      badge.className = 'badge';
      badge.textContent = friendlyType(agent.type);
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
      const run = actionButton({
        className: 'primary agent-action agent-action-run', icon: '▶', label: 'Run',
        title: 'Open a new session and start the task immediately',
        click: () => post({ type: 'run_agent', id: agent.id, effort: effort() })
      });
      const loop = actionButton({
        className: 'toggle agent-action agent-action-loop', icon: '∞', label: 'Loop',
        title: 'Run fresh-context batches until Stop loop is pressed',
        click: event => {
          for (const item of document.querySelectorAll('.agent-action-loop')) item.setAttribute('aria-pressed', 'false');
          event.currentTarget.setAttribute('aria-pressed', 'true');
          post({ type: 'run_agent', id: agent.id, perpetual: true, effort: effort() || 'high' });
          revealStop();
          toast?.('Perpetual loop started. Use Stop loop in the top bar to end it.');
        }
      });
      loop.setAttribute('aria-pressed', 'false');
      const open = actionButton({
        className: 'secondary agent-action', label: 'Open', title: 'Open this agent without starting it',
        click: () => post({ type: 'open_agent', id: agent.id })
      });
      const rename = actionButton({
        className: 'secondary agent-action agent-action-quiet', label: 'Rename', title: 'Rename this agent',
        click: () => {
          const next = window.prompt('Rename agent', agent.name);
          if (next === null) return;
          const trimmed = next.trim();
          if (trimmed && trimmed !== agent.name) post({ type: 'rename_agent', id: agent.id, name: trimmed });
        }
      });
      const remove = actionButton({
        className: 'danger agent-action agent-action-quiet', label: 'Delete', title: 'Delete this agent',
        click: () => {
          if (window.confirm(`Delete "${agent.name}"? This cannot be undone.`))
            post({ type: 'delete_agent', id: agent.id });
        }
      });
      actions.append(run, loop, open, rename, remove);
      card.append(head, actions);
      grid.append(card);
    }
  }

  function renderPermissionOptions(box, presetGroups) {
    if (!box) return;
    const enabled = new Set(presetGroups || []);
    box.replaceChildren();
    for (const group of permissionGroups) {
      const label = document.createElement('label');
      label.className = 'permission-item';
      const checkbox = document.createElement('input');
      checkbox.type = 'checkbox';
      checkbox.className = 'permission-checkbox';
      checkbox.value = group.id;
      checkbox.checked = enabled.has(group.id);
      checkbox.setAttribute('aria-label', group.label);
      const text = document.createElement('span');
      text.textContent = group.label;
      label.append(checkbox, text);
      box.append(label);
    }
  }

  function selectedAllowedTools(box) {
    if (!box) return [];
    const selected = new Set([...box.querySelectorAll('input:checked')].map(input => input.value));
    return permissionGroups.flatMap(group => selected.has(group.id) ? group.tools : []);
  }

  return Object.freeze({
    permissionPresets,
    renderAgents,
    renderPermissionOptions,
    selectedAllowedTools
  });
})();

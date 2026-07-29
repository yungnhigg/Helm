// Minimal markdown renderer.
//
// Builds DOM nodes directly and never touches innerHTML. That is deliberate:
// crawled pages, tool output, and document extracts all reach this function, so
// the renderer must be incapable of producing markup rather than merely
// escaping it carefully. Text always becomes text nodes.
//
// No dependency and no CDN — the app has to work with the network off.
(function (global) {
  'use strict';

  const SAFE_LINK = /^https?:\/\//i;

  // ---- inline: `code`, **bold**, *italic*, [text](url), bare urls
  function inline(text, out) {
    const pattern = /(`[^`\n]+`)|(\*\*[^*\n]+\*\*)|(\*[^*\n]+\*)|(__[^_\n]+__)|(\[[^\]\n]*\]\([^)\s]+\))|(https?:\/\/[^\s<>()]+)/g;
    let last = 0, m;
    while ((m = pattern.exec(text)) !== null) {
      if (m.index > last) out.appendChild(document.createTextNode(text.slice(last, m.index)));
      const tok = m[0];
      if (tok.startsWith('`')) {
        const el = document.createElement('code');
        el.className = 'inline-code';
        el.textContent = tok.slice(1, -1);
        out.appendChild(el);
      } else if (tok.startsWith('**') || tok.startsWith('__')) {
        const el = document.createElement('strong');
        el.textContent = tok.slice(2, -2);
        out.appendChild(el);
      } else if (tok.startsWith('*')) {
        const el = document.createElement('em');
        el.textContent = tok.slice(1, -1);
        out.appendChild(el);
      } else if (tok.startsWith('[')) {
        const split = tok.indexOf('](');
        const label = tok.slice(1, split);
        const href = tok.slice(split + 2, -1);
        out.appendChild(anchor(href, label || href));
      } else {
        out.appendChild(anchor(tok, tok));
      }
      last = m.index + tok.length;
    }
    if (last < text.length) out.appendChild(document.createTextNode(text.slice(last)));
  }

  function anchor(href, label) {
    if (!SAFE_LINK.test(href)) {              // no javascript:, file:, data:
      return document.createTextNode(label);
    }
    const a = document.createElement('a');
    a.className = 'md-link';
    a.textContent = label;
    a.title = href;
    a.addEventListener('click', ev => {
      ev.preventDefault();
      // Navigation is pinned to the app origin, so hand the URL to the shell.
      if (global.chrome && global.chrome.webview) {
        global.chrome.webview.postMessage(JSON.stringify({ type: 'open_external', url: href }));
      }
    });
    return a;
  }

  function codeBlock(lang, code) {
    const wrap = document.createElement('div');
    wrap.className = 'code-block';

    const head = document.createElement('div');
    head.className = 'code-head';
    const tag = document.createElement('span');
    tag.className = 'code-lang';
    tag.textContent = lang || 'text';
    const copy = document.createElement('button');
    copy.className = 'code-copy';
    copy.type = 'button';
    copy.textContent = 'Copy';
    copy.addEventListener('click', () => {
      const done = () => {
        copy.textContent = 'Copied';
        copy.classList.add('copied');
        setTimeout(() => { copy.textContent = 'Copy'; copy.classList.remove('copied'); }, 1400);
      };
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(code).then(done, fallbackCopy);
      } else {
        fallbackCopy();
      }
      function fallbackCopy() {
        const ta = document.createElement('textarea');
        ta.value = code;
        ta.style.position = 'fixed';
        ta.style.opacity = '0';
        document.body.appendChild(ta);
        ta.select();
        try { document.execCommand('copy'); done(); } catch (_) { /* ignore */ }
        document.body.removeChild(ta);
      }
    });
    head.append(tag, copy);

    const pre = document.createElement('pre');
    const el = document.createElement('code');
    el.textContent = code;
    pre.appendChild(el);
    wrap.append(head, pre);
    return wrap;
  }

  function flushList(state, frag) {
    if (!state.list) return;
    frag.appendChild(state.list);
    state.list = null;
    state.listType = null;
  }

  function render(text) {
    const frag = document.createDocumentFragment();
    const lines = String(text == null ? '' : text).split('\n');
    const state = { list: null, listType: null };

    let i = 0;
    let para = [];

    const flushPara = () => {
      if (!para.length) return;
      const p = document.createElement('p');
      p.className = 'md-p';
      inline(para.join('\n'), p);
      frag.appendChild(p);
      para = [];
    };

    while (i < lines.length) {
      const line = lines[i];
      const fence = line.match(/^\s*```+\s*([A-Za-z0-9_+\-#.]*)\s*$/);

      if (fence) {
        flushPara(); flushList(state, frag);
        const lang = fence[1] || '';
        const body = [];
        i++;
        while (i < lines.length && !/^\s*```+\s*$/.test(lines[i])) { body.push(lines[i]); i++; }
        i++; // closing fence (or EOF for a still-streaming block)
        frag.appendChild(codeBlock(lang, body.join('\n')));
        continue;
      }

      const heading = line.match(/^(#{1,4})\s+(.*)$/);
      if (heading) {
        flushPara(); flushList(state, frag);
        const h = document.createElement('div');
        h.className = 'md-h md-h' + heading[1].length;
        inline(heading[2], h);
        frag.appendChild(h);
        i++; continue;
      }

      if (/^\s*(---+|\*\*\*+|___+)\s*$/.test(line)) {
        flushPara(); flushList(state, frag);
        frag.appendChild(document.createElement('hr'));
        i++; continue;
      }

      const quote = line.match(/^\s*>\s?(.*)$/);
      if (quote) {
        flushPara(); flushList(state, frag);
        const bq = document.createElement('blockquote');
        bq.className = 'md-quote';
        inline(quote[1], bq);
        frag.appendChild(bq);
        i++; continue;
      }

      const bullet = line.match(/^\s*[-*+]\s+(.*)$/);
      const numbered = line.match(/^\s*(\d+)[.)]\s+(.*)$/);
      if (bullet || numbered) {
        flushPara();
        const want = bullet ? 'ul' : 'ol';
        if (state.listType !== want) {
          flushList(state, frag);
          state.list = document.createElement(want);
          state.list.className = 'md-list';
          state.listType = want;
        }
        const li = document.createElement('li');
        inline(bullet ? bullet[1] : numbered[2], li);
        state.list.appendChild(li);
        i++; continue;
      }

      if (!line.trim()) { flushPara(); flushList(state, frag); i++; continue; }

      para.push(line);
      i++;
    }

    flushPara();
    flushList(state, frag);
    return frag;
  }

  global.renderMarkdown = render;
})(window);

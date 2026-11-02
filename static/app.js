'use strict';
const $ = selector => document.querySelector(selector);
const esc = value => String(value).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const jobId = id => `J${String(id).padStart(3, '0')}`;
const isActive = job => !['Completed', 'Cancelled'].includes(job.status);
let state = null, filter = 'active', busy = false;
function notice(message, error = false) { $('#notice').textContent = message; $('#notice').classList.toggle('error', error); $('#notice').hidden = false; }
async function refresh() {
  const response = await fetch('/api/state');
  if (!response.ok) throw new Error('Cannot reach the workshop server');
  state = await response.json(); render();
  $('#connection').textContent = '● Live · C server connected';
}
async function change(path, payload = {}) {
  if (busy) return false;
  busy = true; syncButtons();
  try {
    const response = await fetch(path, {method: 'POST', headers: {'Content-Type': 'application/json', 'X-MakerLab': '1'}, body: JSON.stringify(payload)});
    const result = await response.json();
    if (!response.ok) throw new Error(result.error || 'Action failed');
    notice(result.message); await refresh(); return true;
  } catch (error) { notice(error.message, true); $('#form-error').textContent = error.message; return false; }
  finally { busy = false; syncButtons(); }
}
function syncButtons() {
  document.querySelectorAll('[data-action], #dispatch, #demo, #project-form button[type="submit"]').forEach(button => { button.disabled = busy; });
  if (state) $('#demo').disabled = busy || state.jobs.some(isActive);
}
function render() {
  const active = state.jobs.filter(isActive);
  $('#active-count').textContent = active.length;
  $('#running-count').textContent = state.jobs.filter(j => j.status === 'Running').length;
  $('#complete-count').textContent = state.jobs.filter(j => j.status === 'Completed').length;
  $('#safe-label').textContent = state.safe_sequence === null ? 'Unsafe' : 'Safe state';
  $('#queue-count').textContent = active.length;
  $('#sequence').textContent = state.safe_sequence === null ? 'No safe sequence exists' : state.safe_sequence.length ? state.safe_sequence.map(jobId).join('  →  ') : 'All resources available. Add a project to begin.';
  $('#resources').innerHTML = state.resources.map((name,i) => `<div class="resource"><div class="resource-top"><strong>${esc(name)}</strong><span>${state.available[i]} / ${state.capacity[i]} free</span></div><progress max="${state.capacity[i]}" value="${state.capacity[i]-state.available[i]}" aria-label="${esc(name)} units held"></progress><small>${state.capacity[i]-state.available[i]} held · ${active.filter(j=>j.allocation[i]>0).map(j=>jobId(j.id)).join(', ') || 'No reservations'}</small></div>`).join('');
  $('#matrix').innerHTML = active.length ? active.map(j => `<tr><td>${jobId(j.id)}</td><td>[${j.maximum.join(', ')}]</td><td>[${j.allocation.join(', ')}]</td><td>[${j.need.join(', ')}]</td></tr>`).join('') : '<tr><td colspan="4">No active claims.</td></tr>';
  $('#events').innerHTML = state.events.length ? state.events.map(e => `<div class="event ${esc(e.kind)}"><time>${new Date(e.at*1000).toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'})}</time><span class="event-kind">${esc(e.kind)}</span><span class="event-message">${esc(e.message)}</span></div>`).join('') : '<div class="empty"><p>Every decision leaves a trace. Your workshop activity will appear here.</p></div>';
  renderJobs(); syncButtons();
}
function renderJobs() {
  if (!state) return;
  const search = $('#search').value.toLowerCase();
  const jobs = state.jobs.filter(j => (filter === 'all' || (filter === 'active' ? isActive(j) : !isActive(j))) && `${j.title} ${j.owner} ${jobId(j.id)}`.toLowerCase().includes(search));
  jobs.sort((a,b) => b.effective_priority-a.effective_priority || a.queued_at-b.queued_at || a.id-b.id);
  $('#job-list').innerHTML = jobs.length ? jobs.map(j => `<article class="job"><div class="job-head"><div><h3>${esc(j.title)}</h3><div class="job-subtitle"><b>${jobId(j.id)}</b> ${esc(j.owner)} · Priority ${j.effective_priority}${j.effective_priority>j.priority ? ` (+${j.effective_priority-j.priority} aging)` : ''}</div></div><span class="badge ${j.status}">${j.status}</span></div><div class="job-bottom"><div class="stage-track">${['Preparation','Fabrication','Inspection'].map((s,i) => `<span class="${i===j.stage?'current':i<j.stage?'done':''}">${i<j.stage?'✓ ':''}${s}</span>`).join('<span class="arrow">›</span>')}</div><div class="job-actions">${j.status==='Ready'?`<button data-action="start" data-id="${j.id}">Start stage →</button>`:''}${j.status==='Running'?`<button data-action="complete" data-id="${j.id}">Complete stage ✓</button>`:''}${isActive(j)?`<button class="cancel" data-action="cancel" data-id="${j.id}" aria-label="Cancel ${esc(j.title)}">Cancel</button>`:''}</div></div></article>`).join('') : `<div class="empty"><span class="empty-symbol">◇</span><h3>${search?'No matching projects':filter==='closed'?'A fresh workshop':'Ready for your next idea'}</h3><p>${search?'Try a different project or team name.':filter==='closed'?'Completed and cancelled projects will appear here.':'Add a project to begin, or load the safety demo to see Banker’s Algorithm in action.'}</p></div>`;
}
$('#new-job').addEventListener('click', () => { $('#form-error').textContent = ''; $('#project-dialog').showModal(); });
$('#close-dialog').addEventListener('click', () => $('#project-dialog').close());
$('#project-form').addEventListener('submit', async event => {
  event.preventDefault(); const form = new FormData(event.target);
  if (await change('/api/jobs', {title: form.get('title'), owner: form.get('owner'), priority: Number(form.get('priority'))})) {
    $('#project-dialog').close(); event.target.reset();
  }
});
$('#dispatch').addEventListener('click', () => change('/api/dispatch'));
$('#demo').addEventListener('click', () => change('/api/demo'));
$('#job-list').addEventListener('click', event => {
  const button = event.target.closest('[data-action]'); if (!button) return;
  if (button.dataset.action === 'cancel' && !confirm(`Cancel ${jobId(button.dataset.id)} and release its resources?`)) return;
  change(`/api/jobs/${button.dataset.id}/${button.dataset.action}`);
});
$('#search').addEventListener('input', renderJobs);
document.querySelectorAll('[data-filter]').forEach(button => button.addEventListener('click', () => {
  filter = button.dataset.filter;
  document.querySelectorAll('[data-filter]').forEach(b => { b.classList.toggle('active', b===button); b.setAttribute('aria-pressed', String(b===button)); });
  renderJobs(); syncButtons();
}));
document.querySelectorAll('nav a').forEach(link => link.addEventListener('click', () => { document.querySelectorAll('nav a').forEach(a => a.classList.toggle('selected',a===link)); }));
$('#export').addEventListener('click', () => {
  if (!state) return;
  const url = URL.createObjectURL(new Blob([JSON.stringify(state,null,2)], {type:'application/json'}));
  const link = document.createElement('a'); link.href=url; link.download='makerlab-snapshot.json'; link.click(); setTimeout(()=>URL.revokeObjectURL(url),1000);
});
async function poll() {
  try { if (!busy) await refresh(); } catch (error) { $('#connection').textContent='● Offline · retrying'; notice(error.message, true); }
  setTimeout(poll, 5000);
}
poll();

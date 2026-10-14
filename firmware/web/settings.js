'use strict';
const form = document.querySelector('#settings-form');
const controls = document.querySelector('#controls');
const save = document.querySelector('#save');
const saveBar = document.querySelector('.save-bar');
const notice = document.querySelector('#notice');
let ownNight = false;  // Mario and Luigi paint their own night from the sun: no night schedule, only the dark-room clock
let initial = '', busy = false, saving = false, savedSwap = false, saveTimer = 0, noticeTimer = 0;
const field = name => form.elements.namedItem(name);
const encoded = () => new URLSearchParams(new FormData(form));
// Everything applies live except the panel colour swap, which only takes effect after a restart:
// automatic saves keep sending the last saved value for it.
function payload(withRestart) {
  const params = encoded();
  if (!withRestart) { if (savedSwap) params.set('swapBlueGreen', 'on'); else params.delete('swapBlueGreen'); }
  return params;
}
function message(text, error = false) { clearTimeout(noticeTimer); notice.textContent = text; notice.classList.toggle('error', error); notice.hidden = false; }
function brightness(event) {
  const high = field('brightness'), low = field('minimumBrightness'), auto = field('autoBrightness').checked;
  if (auto) {
    if (event && event.target === low) high.value = Math.max(Number(high.value), Number(low.value));
    else low.value = Math.min(Number(low.value), Number(high.value));
  }
  const pct = input => Math.round(Number(input.value) / 255 * 100) + '%';
  document.querySelector('#brightness-value').textContent = auto ? pct(low) + ' – ' + pct(high) : pct(high);
  document.querySelector('#brightness-dual').classList.toggle('auto', auto);
  const dual = document.querySelector('#brightness-dual');
  dual.style.setProperty('--lo', auto ? (Number(low.value) - 1) / 254 : 0);
  dual.style.setProperty('--hi', (Number(high.value) - 1) / 254);
  const nightOn = field('nightEnabled').checked;
  document.querySelector('#night-fields').hidden = !nightOn && !ownNight;
  document.querySelector('#night-schedule').hidden = ownNight;
  document.querySelector('#night-sun').hidden = ownNight;
  document.querySelector('#own-night').hidden = !ownNight;
  document.querySelector('#night-times').hidden = field('nightSun').checked || ownNight;
  const sunHint = document.querySelector('#sun-hint'); sunHint.hidden = ownNight || !field('nightSun').checked || !sunHint.textContent;
  const night = field('nightBrightness');
  document.querySelector('#night-dual').style.setProperty('--hi', (Number(night.value) - 1) / 254);
  document.querySelector('#night-value').textContent = pct(night);
  document.querySelector('#range-low').textContent = auto ? 'Dimmest' : 'Minimum';
}
let lightNow = -1;
function sensor(event) {
  const dark = field('ldrDark'), light = field('ldrLight');
  if (event && event.target === light) dark.value = Math.min(Number(dark.value), Math.max(Number(light.value) - 1, 0));
  else light.value = Math.max(Number(light.value), Number(dark.value) + 1);
  if (Number(dark.value) >= Number(light.value)) dark.value = Math.max(Number(light.value) - 1, 0);
  const box = document.querySelector('#sensor-dual'), mark = document.querySelector('#sensor-mark');
  box.style.setProperty('--lo', Number(dark.value) / 4095);
  box.style.setProperty('--hi', Number(light.value) / 4095);
  document.querySelector('#sensor-range').textContent = dark.value + ' – ' + light.value;
  mark.hidden = lightNow < 0; box.style.setProperty('--mark', Math.min(lightNow, 4095) / 4095);
  document.querySelector('#sensor-reading').textContent = lightNow >= 0 ? 'Current sensor reading: ' + lightNow : 'Sensor is off.';
}
function timezoneMode() {
  const auto = field('autoTimezone').checked;
  field('timezone').disabled = auto; document.querySelector('#device-timezone').disabled = auto;
  document.querySelector('#timezone-field').style.opacity = auto ? 0.45 : 1;
}
function dirty() {
  if (busy) return;
  const pending = field('swapBlueGreen').checked !== savedSwap;
  document.querySelector('#save-status').textContent = 'Restart needed to apply the colour swap';
  save.hidden = !pending; saveBar.hidden = !pending;
  if (payload(false).toString() !== initial) { clearTimeout(saveTimer); saveTimer = setTimeout(autosave, 700); }
}
async function autosave() {
  if (busy || controls.disabled) return;
  if (saving) { saveTimer = setTimeout(autosave, 300); return; }
  if (!form.checkValidity()) return;  // wait until the field is valid; no popups while typing
  const body = payload(false), sent = body.toString();
  if (sent === initial) return;
  saving = true;
  try {
    const response = await fetch('/api/settings', { method: 'POST', body });
    const result = await response.json();
    if (!response.ok) throw new Error(result.message || 'The clock could not save your settings.');
    initial = sent; message('Saved'); noticeTimer = setTimeout(() => { notice.hidden = true; }, 2000);
    if (payload(false).toString() !== initial) saveTimer = setTimeout(autosave, 300);
  } catch (error) { message(error.message, true); }
  saving = false;
}
function section(selector, show) { document.querySelectorAll(selector).forEach(node => { node.hidden = !show; node.querySelectorAll('input,select').forEach(input => input.disabled = !show); }); }
function choices(id, name, entries, values) {
  const selected = new Set((values || '').split(','));
  const target = document.querySelector(id);
  const grouped = entries.some(entry => entry.group);
  target.replaceChildren();
  let group = null;
  // Europe and national teams stay on top, then countries with a selection, then A-Z (stable sort keeps league order)
  const pinned = ['Europe', 'National teams'];
  const rank = group => pinned.includes(group) ? pinned.indexOf(group) - 2 : entries.some(entry => entry.group === group && selected.has(entry.value)) ? 0 : 1;
  (grouped ? [...entries].sort((a, b) => a.group === b.group ? 0 : rank(a.group) - rank(b.group) || a.group.localeCompare(b.group)) : [...entries].sort((a, b) => selected.has(b.value) - selected.has(a.value))).forEach(entry => {
    if (grouped && entry.group !== group) {
      group = entry.group; const heading = document.createElement('div'); heading.className = 'choice-group'; heading.dataset.group = group; heading.textContent = group;
      heading.setAttribute('role', 'button'); heading.tabIndex = 0;
      const toggle = () => foldGroup(target, heading.dataset.group, heading.getAttribute('aria-expanded') === 'true');
      heading.addEventListener('click', toggle);
      heading.addEventListener('keydown', event => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault(); toggle(); } });
      target.append(heading);
    }
    const label = document.createElement('label'); label.className = 'choice';
    const input = document.createElement('input'); input.type = 'checkbox'; input.name = name; input.value = entry.value; input.checked = selected.has(entry.value);
    const text = document.createElement('span'); text.textContent = entry.label;
    label.dataset.search = (entry.label + ' ' + (entry.group || '')).toLowerCase();
    if (grouped) label.dataset.group = entry.group;
    label.append(input, text); target.append(label);
  });
  // Countries and regions start folded, unless something in them is selected
  if (grouped) target.querySelectorAll('.choice-group').forEach(heading => {
    const group = heading.dataset.group;
    foldGroup(target, group, ![...target.querySelectorAll('label.choice')].some(label => label.dataset.group === group && label.querySelector('input').checked));
  });
}
function foldGroup(target, group, fold) {
  target.querySelectorAll('.choice-group').forEach(heading => { if (heading.dataset.group === group) heading.setAttribute('aria-expanded', String(!fold)); });
  target.querySelectorAll('label.choice').forEach(label => { if (label.dataset.group === group) label.classList.toggle('folded', fold); });
}
const TEAM_LIMIT = 8;
let teamCatalog = [], favourites = [];
const parseTeams = text => (text || '').split(',').filter(Boolean).map(item => { const [id, abbr, kind] = [item.slice(0, item.indexOf(':')), item.slice(item.indexOf(':') + 1, item.lastIndexOf(':')), item.slice(item.lastIndexOf(':') + 1)]; return { id, abbr, kind }; });
function teamName(team) { const known = teamCatalog.find(entry => String(entry[0]) === team.id); return known ? known[2] : team.abbr; }
let suggestions = [], active = 0;
function renderTeams() {
  const input = document.querySelector('#team-search'), tags = document.querySelector('#team-tags'), box = document.querySelector('#team-suggest');
  const query = input.value.trim().toLowerCase(), chosen = new Set(favourites.map(team => team.id));
  tags.replaceChildren(...favourites.map(team => {
    const tag = document.createElement('span'); tag.className = 'tag'; tag.append(teamName(team) + (team.kind === 'n' ? ' 🏳' : ''));
    const remove = document.createElement('button'); remove.type = 'button'; remove.className = 'tag-x'; remove.textContent = '×'; remove.setAttribute('aria-label', 'Remove ' + teamName(team));
    remove.addEventListener('click', () => { removeTeam(team.id); input.focus(); });
    tag.append(remove); return tag;
  }));
  suggestions = query ? teamCatalog.filter(entry => !chosen.has(String(entry[0])) && (entry[2].toLowerCase().includes(query) || entry[1].toLowerCase() === query)).sort((a, b) => b[2].toLowerCase().startsWith(query) - a[2].toLowerCase().startsWith(query)).slice(0, 8) : [];
  active = Math.min(active, Math.max(suggestions.length - 1, 0));
  box.replaceChildren(...suggestions.map((entry, i) => {
    const item = document.createElement('div'); item.className = 'suggest-item' + (i === active ? ' active' : ''); item.setAttribute('role', 'option');
    item.textContent = entry[2] + (entry[3] === 'n' ? ' (national team)' : '');
    item.addEventListener('mousedown', event => { event.preventDefault(); addTeam(entry); });
    return item;
  }));
  if (query && !suggestions.length) { const none = document.createElement('div'); none.className = 'suggest-none'; none.textContent = 'No team found'; box.append(none); }
  box.hidden = !query; input.setAttribute('aria-expanded', String(!!query));
  document.querySelector('#team-count').textContent = favourites.length + ' of ' + TEAM_LIMIT + ' selected';
}
function saveTeams() { field('fbTeams').value = favourites.map(team => team.id + ':' + team.abbr + ':' + team.kind).join(','); renderTeams(); dirty(); }
function addTeam(entry) {
  if (favourites.length >= TEAM_LIMIT) { message('Choose up to ' + TEAM_LIMIT + ' teams. Remove one to add another.', true); return; }
  favourites.push({ id: String(entry[0]), abbr: entry[1], kind: entry[3] });
  document.querySelector('#team-search').value = ''; active = 0; saveTeams();
}
function removeTeam(id) { favourites = favourites.filter(team => team.id !== id); saveTeams(); }
function countLeagues() { const count = form.querySelectorAll('[name=fbLeagues]:checked').length; document.querySelector('#league-count').textContent = count + ' of 10 selected'; }
// Birthdays: one hidden "MMDD:NAME:YYYY,..." field, edited as rows; "_" stands for a space in a name
const bdayList = document.querySelector('#bday-list'), bdayAdd = document.querySelector('#bday-add');
const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
const daysIn = month => [31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][month - 1];
const bdayName = value => value.normalize('NFD').replace(/[\u0300-\u036f]/g, '').toUpperCase().replace(/[^A-Z0-9 ]/g, '').slice(0, 12);  // what the panel's small font can draw
function fillDays(select, month, day) { select.replaceChildren(...Array.from({ length: daysIn(month) }, (_, i) => new Option(i + 1, i + 1))); select.value = Math.min(day, daysIn(month)); }
function bdayRow(b) {
  const part = (tag, cls, label) => { const el = document.createElement(tag); el.className = cls; el.setAttribute('aria-label', label); return el; };
  const row = part('div', 'bday', 'Birthday');
  const name = part('input', 'n', 'Name'), day = part('select', 'd', 'Day'), month = part('select', 'm', 'Month'), year = part('input', 'y', 'Birth year (optional)'), drop = part('button', 'x', 'Remove');
  name.type = 'text'; name.maxLength = 12; name.placeholder = 'Name'; name.autocomplete = 'off'; name.value = b.name;
  MONTHS.forEach((label, i) => month.add(new Option(label, i + 1))); month.value = b.month; fillDays(day, b.month, b.day);
  year.type = 'text'; year.inputMode = 'numeric'; year.maxLength = 4; year.placeholder = 'Year'; year.value = b.year;
  drop.type = 'button'; drop.textContent = '\u00d7';
  row.append(name, day, month, year, drop);
  return row;
}
function bdayCount() { bdayAdd.hidden = bdayList.children.length >= 10; const named = field('birthdays').value ? field('birthdays').value.split(',').length : 0; document.querySelector('#bday-count').textContent = named ? named + (named === 1 ? ' birthday' : ' birthdays') : ''; }
function bdaySync() {
  field('birthdays').value = [...bdayList.children].map(row => {
    const name = bdayName(row.querySelector('.n').value).trim();
    return name ? String(row.querySelector('.m').value).padStart(2, '0') + String(row.querySelector('.d').value).padStart(2, '0') + ':' + name.replace(/ /g, '_') + ':' + row.querySelector('.y').value.replace(/\D/g, '') : '';
  }).filter(Boolean).join(',');
  bdayCount();
}
function loadBirthdays(text) {
  bdayList.replaceChildren(...text.split(',').filter(Boolean).map(entry => { const [date = '', name = '', year = ''] = entry.split(':'); return bdayRow({ month: Number(date.slice(0, 2)) || 1, day: Number(date.slice(2)) || 1, name: name.replace(/_/g, ' '), year }); }));
  bdayCount();
}
bdayList.addEventListener('input', event => {
  const el = event.target;
  if (el.classList.contains('n')) { const clean = bdayName(el.value); if (clean !== el.value) el.value = clean; }
  bdaySync();
});
bdayList.addEventListener('change', event => {
  const row = event.target.closest('.bday');
  if (event.target.classList.contains('m')) fillDays(row.querySelector('.d'), Number(event.target.value), Number(row.querySelector('.d').value));
  bdaySync();
});
bdayList.addEventListener('click', event => { if (event.target.classList.contains('x')) { event.target.closest('.bday').remove(); bdaySync(); field('birthdays').dispatchEvent(new Event('input', { bubbles: true })); } });
bdayAdd.addEventListener('click', () => { const now = new Date(); bdayList.append(bdayRow({ month: now.getMonth() + 1, day: now.getDate(), name: '', year: '' })); bdayCount(); bdayList.lastChild.querySelector('.n').focus(); });
async function load() {
  try {
    const response = await fetch('/api/settings', { cache: 'no-store' });
    if (!response.ok) throw new Error('The clock could not load its settings. Reload this page to try again.');
    const data = await response.json();
    const racing = data.mode === 'F1' || data.mode === 'MotoGP', sporty = racing || data.mode === 'Football';
    const pacman = data.mode === 'Pac-Man';
    section('[data-sports]', sporty || data.mode === 'Words' || data.mode === 'Mario' || data.mode === 'Luigi' || pacman); section('[data-sportfields]', sporty); section('[data-word]', data.mode === 'Words'); section('[data-mario]', data.mode === 'Mario' || data.mode === 'Luigi'); section('[data-pacman]', pacman);
    section('[data-football]', data.mode === 'Football'); section('[data-f1]', racing);
    if (sporty || data.mode === 'Words' || data.mode === 'Mario' || data.mode === 'Luigi' || pacman) { document.querySelector('.sidebar nav a.active')?.classList.remove('active'); document.querySelector('.sidebar nav a[href="#mode"]').classList.add('active'); }
    if (pacman) {
      const colours = ['Default', 'Pastel green', 'Pastel pink', 'Pastel blue', 'Lemon', 'Peach', 'Pastel purple', 'Red', 'Orange', 'Cyan', 'White'];
      for (const name of ['ghost1Color', 'ghost2Color', 'ghost3Color', 'ghost4Color', 'dotColor', 'pacmanClockColor']) field(name).replaceChildren(...colours.map((label, value) => new Option(label, value)));
    }
    if (data.mode === 'MotoGP') document.querySelector('[data-f1] .field > label').textContent = 'Favourite riders';
    if (racing) form.querySelector('[name=timeStyle] option[value="0"]').remove();
    Object.entries(data.settings).forEach(([name, value]) => { const input = field(name); if (!input) return; if (input.tagName === 'SELECT' && ![...input.options].some(o => o.value === String(value))) input.add(new Option(value, value)); if (input.type === 'checkbox') input.checked = Boolean(value); else input.value = name === 'use24h' ? (value ? '1' : '0') : name === 'timeStyle' && racing && Number(value) === 0 ? 2 : value; });
    loadBirthdays(field('birthdays').value);
    document.querySelector('#sun-hint').textContent = data.sunTimes || '';
    document.querySelector('#mode-name').textContent = data.mode; currentFace = data.modeId; markFace();
    document.querySelector('#sport-heading').textContent = data.mode + ' preferences';
    document.querySelector('#network-name').textContent = data.network.ssid || 'Not connected';
    document.querySelector('#network-ip').textContent = data.network.ip || '—';
    document.querySelector('#sidebar-ip').textContent = data.network.ip || 'Offline';
    if (data.welcomeHidden && !data.welcomeMode) hideWelcome();
    document.body.classList.toggle('welcome-only', !!data.welcomeMode);
    { const start = document.querySelector('#welcome-start'); if (start) start.hidden = !data.welcomeMode; const dismiss = document.querySelector('#welcome-dismiss'); if (dismiss) dismiss.parentElement.hidden = !!data.welcomeMode; }
    { const link = document.querySelector('#welcome-ip'); const url = data.network.ip ? 'http://' + data.network.ip + '/' : ''; if (link) { link.textContent = url || '—'; link.href = url || '#'; } }
    lightNow = data.lightReading;
    document.querySelector('#time-status').textContent = data.timeSynced ? 'Time is synced automatically' : 'Waiting for the first time sync';
    if (sporty || data.mode === 'Words') {
      const catalogResponse = await fetch('/api/catalog');
      if (!catalogResponse.ok) throw new Error('The selection list could not load. Reload this page before saving.');
      const catalog = await catalogResponse.json();
      if (data.mode === 'Football') {
        choices('#leagues', 'fbLeagues', catalog.map(([value, , label, group]) => ({ value, label, group })), data.settings.fbLeagues);
        const teamResponse = await fetch('/api/teams');
        if (!teamResponse.ok) throw new Error('The team list could not load. Reload this page before saving.');
        teamCatalog = await teamResponse.json(); favourites = parseTeams(data.settings.fbTeams); renderTeams();
      }
      else if (data.mode === 'Words') {
        const select = field('wordLanguage');
        select.replaceChildren(...catalog.map(entry => new Option(entry.label, entry.value)));
        select.value = data.settings.wordLanguage;
      } else choices('#drivers', 'f1Drivers', catalog, data.settings.f1Drivers);
    }
    controls.disabled = false; save.disabled = false; savedSwap = field('swapBlueGreen').checked; ownNight = data.mode === 'Mario' || data.mode === 'Luigi'; brightness(); sensor(); timezoneMode(); countLeagues(); initial = payload(false).toString(); dirty();
    if (sessionStorage.getItem('ultimatrix-saved')) { sessionStorage.removeItem('ultimatrix-saved'); message('Settings saved. Your clock is ready.'); }
  } catch (error) { message(error.message, true); document.querySelector('#save-status').textContent = 'Settings could not load'; saveBar.hidden = false; }
}
form.addEventListener('input', event => { brightness(event); sensor(event); timezoneMode(); if (!/^(text|search|url)$/.test(event.target.type)) dirty(); });  // typed text saves on change (blur)
for (const [id, name] of [['#set-dark', 'ldrDark'], ['#set-light', 'ldrLight']]) {
  document.querySelector(id).addEventListener('click', () => { if (lightNow < 0) return; field(name).value = lightNow; sensor({ target: field(name) }); dirty(); });
}
setInterval(async () => {
  if (document.hidden || !field('autoBrightness').checked || controls.disabled) return;
  try { const data = await (await fetch('/api/settings', { cache: 'no-store' })).json(); lightNow = data.lightReading; sensor(); } catch (error) { /* next poll retries */ }
}, 2000);
form.addEventListener('change', event => {
  if (event.target.name === 'fbLeagues') {
    if (form.querySelectorAll('[name=fbLeagues]:checked').length > 10) { event.target.checked = false; message('Choose up to 10 competitions. Unselect one to add another.', true); }
    countLeagues();
  }
  dirty();
});
document.querySelector('#league-search').addEventListener('input', event => {
  const query = event.target.value.toLowerCase(); let heading = null, shown = 0;
  document.querySelector('#leagues').classList.toggle('searching', query !== '');  // folded groups open up while searching
  const finish = () => { if (heading) heading.hidden = !shown; };
  document.querySelectorAll('#leagues > *').forEach(node => {
    if (node.classList.contains('choice-group')) { finish(); heading = node; shown = 0; return; }
    node.hidden = !node.dataset.search.includes(query); if (!node.hidden) shown++;
  });
  finish();
});
const teamInput = document.querySelector('#team-search');
teamInput.addEventListener('input', () => { active = 0; renderTeams(); });
teamInput.addEventListener('blur', () => { teamInput.value = ''; renderTeams(); });
teamInput.addEventListener('keydown', event => {
  if (event.key === 'ArrowDown' || event.key === 'ArrowUp') { event.preventDefault(); if (suggestions.length) { active = (active + (event.key === 'ArrowDown' ? 1 : suggestions.length - 1)) % suggestions.length; renderTeams(); } }
  else if (event.key === 'Enter' || event.key === ',') { event.preventDefault(); if (suggestions[active]) addTeam(suggestions[active]); }
  else if (event.key === 'Escape') { teamInput.value = ''; renderTeams(); }
  else if (event.key === 'Backspace' && !teamInput.value && favourites.length) removeTeam(favourites[favourites.length - 1].id);
});
document.querySelector('#team-box').addEventListener('click', () => teamInput.focus());
document.querySelector('#device-timezone').addEventListener('click', () => {
  const zone = Intl.DateTimeFormat().resolvedOptions().timeZone;
  if (!zone) { message('Device timezone unavailable. Choose a timezone manually.', true); return; }
  field('timezone').value = zone;
  dirty();
  message('Device timezone selected: ' + zone + '.');
});
let zonesLoaded = false;
field('timezone').addEventListener('focus', async () => {
  if (zonesLoaded) return;
  try {
    const response = await fetch('/api/timezones'); if (!response.ok) return;
    const zones = await response.json();
    const list = document.querySelector('#timezones'); list.replaceChildren();
    zones.forEach(zone => { const option = document.createElement('option'); option.value = zone; list.append(option); }); zonesLoaded = true;
  } catch (_) { /* The existing suggestions and server validation remain available. */ }
});
form.addEventListener('submit', async event => {
  event.preventDefault(); if (busy || !form.reportValidity()) return;
  clearTimeout(saveTimer); while (saving) await new Promise(resolve => setTimeout(resolve, 100));
  const body = payload(true); busy = true; save.disabled = true; save.textContent = 'Saving…';
  try {
    const response = await fetch('/api/settings', { method: 'POST', body });
    const result = await response.json();
    if (!response.ok) throw new Error(result.message || 'The clock could not save your settings.');
    if (!result.restart) { busy = false; save.disabled = false; save.textContent = 'Save & restart'; savedSwap = field('swapBlueGreen').checked; initial = payload(false).toString(); dirty(); message(result.message); return; }
    controls.disabled = true; save.textContent = 'Restarting…'; document.querySelector('#save-status').textContent = 'Settings saved'; saveBar.hidden = false; message('Saved. Your clock is restarting. This page will reconnect automatically.');
    sessionStorage.setItem('ultimatrix-saved', '1');
    setTimeout(reconnect, 2500);
  } catch (error) { busy = false; save.disabled = false; save.textContent = 'Save & restart'; message(error.message, true); dirty(); }
});
async function reconnect() {
  const deadline = Date.now() + 45000;
  while (Date.now() < deadline) {
    try { const response = await fetch('/api/settings', { cache: 'no-store', signal: typeof AbortSignal.timeout === 'function' ? AbortSignal.timeout(2500) : undefined }); if (response.ok) { location.reload(); return; } } catch (_) {}
    await new Promise(resolve => setTimeout(resolve, 1500));
  }
  message('Your settings were saved. The clock is taking longer to reconnect. Reload this page once it is back online.');
}
const dialog = document.querySelector('#wifi-dialog');
document.querySelector('#change-wifi').addEventListener('click', () => { if (!busy) dialog.showModal(); });
document.querySelector('#wifi-cancel').addEventListener('click', () => dialog.close());
document.querySelector('#wifi-confirm').addEventListener('click', async () => {
  const button = document.querySelector('#wifi-confirm'); button.disabled = true;
  try { const response = await fetch('/api/wifi/setup', { method: 'POST' }); const result = await response.json(); if (!response.ok) throw new Error(result.message); dialog.close(); busy = true; controls.disabled = true; save.disabled = true; message('Your clock is restarting into Wi-Fi setup. Join the Ultimatrix network and open 192.168.4.1, or scan the QR code on the clock.'); }
  catch (error) { dialog.close(); message(error.message || 'Could not open Wi-Fi setup. Try again.', true); button.disabled = false; }
});
document.querySelectorAll('.sidebar nav a').forEach(link => link.addEventListener('click', () => { if (link.hash === '#birthdays') document.querySelector('#birthdays details').open = true; document.querySelector('.sidebar nav a.active')?.classList.remove('active'); link.classList.add('active'); }));
load();

let updatePolling = false, currentFace = '';
async function refreshFirmware() {
  try {
    const response = await fetch('/api/firmware', {cache:'no-store'}); if (!response.ok) return;
    const state = await response.json();
    document.querySelector('#firmware-version').textContent = state.version;
    { const field = document.querySelector('#update-server'); if (document.activeElement !== field) field.value = state.server || ''; }
    document.querySelector('#firmware-memory').textContent = Math.round(state.freeHeap / 1024) + ' KiB';
    document.querySelector('#firmware-status').textContent = state.message;
    document.querySelector('#firmware-latest').textContent = state.checking ? 'Checking…' : state.available ? state.latest + ' available' : state.checkedAt ? 'Up to date' : '—';
    document.querySelector('#firmware-checked').textContent = state.checkedAt ? new Date(state.checkedAt * 1000).toLocaleString() : 'Not yet';
    document.querySelector('#firmware-summary').textContent = state.checking ? 'Checking for updates…' : state.checkError ? 'Last check failed: ' + state.checkError : state.available ? 'Version ' + state.latest + ' is available. Install it from the banner at the top, or leave it for later.' : state.checkedAt ? 'You are up to date (' + state.version + ').' : 'Running ' + state.version + '. Not checked yet.';
    const banner = document.querySelector('#update-banner'), install = document.querySelector('#update-banner-install');
    banner.hidden = !(state.available || state.busy) || state.ready;
    document.querySelector('#update-banner-text').textContent = state.busy ? state.message : 'Version ' + state.latest + ' is ready. You are on ' + state.version + '.';
    install.hidden = state.busy || state.ready;
    document.querySelector('#nav-update-dot').hidden = !state.available;
    document.querySelector('#firmware-check').disabled = state.busy || state.checking;
    if (state.checking && !updatePolling) setTimeout(refreshFirmware, 1500);
    const progress = document.querySelector('#firmware-progress');
    progress.hidden = !state.busy || !state.total; progress.value = state.total ? state.received / state.total * 100 : 0;
    document.querySelector('#firmware-update').disabled = state.busy || state.ready || !state.available;
    if (state.ready) { busy = true; controls.disabled = true; save.disabled = true; message('Firmware verified. Your clock is restarting.'); setTimeout(reconnect, 2500); updatePolling = false; }
    else if (state.lowMemory && updatePolling) { updatePolling = false; document.querySelector('#firmware-update').disabled = false; offerReboot(state.message).catch(error => message(error.message, true)); }
    else if (state.busy) { updatePolling = true; setTimeout(refreshFirmware, 1200); }
    else updatePolling = false;
  } catch (_) { if (updatePolling) setTimeout(refreshFirmware, 1500); }
}
let updateMode = '';
// The install question already warned about the restart, so low memory restarts straight away.
async function offerReboot(text) {
  document.querySelector('#firmware-status').textContent = text;
  const reboot = await fetch('/api/firmware/reboot' + (updateMode ? '?mode=' + encodeURIComponent(updateMode) : ''), {method:'POST'}); const result = await reboot.json();
  if (!reboot.ok) throw new Error(result.message);
  busy = true; controls.disabled = true; save.disabled = true; message('Restarting to install the update. This page will reconnect automatically.'); setTimeout(reconnect, 2500);
}
const liveQuestion = 'A live match or session is running. Pause its live data for a moment and continue anyway?';
async function startUpdate(question, mode, force) {
  if (busy || updatePolling || (!force && !window.confirm(question))) return;
  updateMode = mode || '';
  try {
    const query = new URLSearchParams(); if (mode) query.set('mode', mode); if (force) query.set('force', '1');
    const response = await fetch('/api/firmware' + (query.size ? '?' + query : ''), {method:'POST'}); const result = await response.json();
    if (response.status === 409 && result.live) { if (window.confirm(liveQuestion)) await startUpdate(question, mode, true); else document.querySelector('#firmware-status').textContent = result.message; return; }
    if (response.status === 409 && result.lowMemory) { await offerReboot(result.message); return; }
    if (!response.ok) throw new Error(result.message);
    updatePolling = true; document.querySelector('#firmware-update').disabled = true;
    document.querySelector('#firmware-status').textContent = result.message; setTimeout(refreshFirmware, 500);
  } catch (error) { message(error.message || 'Could not start the update.', true); }
}
const installQuestion = 'Download and install the signed update? Your clock will restart. Unsaved settings will be discarded.';
document.querySelector('#firmware-update').addEventListener('click', () => startUpdate(installQuestion));
document.querySelector('#update-banner-install').addEventListener('click', () => startUpdate(installQuestion));
document.querySelector('#update-server-save').addEventListener('click', async () => {
  const body = new URLSearchParams({url: document.querySelector('#update-server').value.trim()});
  try { const response = await fetch('/api/firmware/server', {method:'POST', body}); const result = await response.json(); document.querySelector('#firmware-status').textContent = result.message; } catch (_) {}
  setTimeout(refreshFirmware, 500);
});
async function runCheck(force) {
  document.querySelector('#firmware-check').disabled = true;
  try {
    const response = await fetch('/api/firmware/check' + (force ? '?force=1' : ''), {method:'POST'}); const result = await response.json();
    document.querySelector('#firmware-status').textContent = result.message;
    if (result.live && !force && window.confirm(liveQuestion)) return runCheck(true);
  } catch (_) {}
  setTimeout(refreshFirmware, 1200);
}
document.querySelector('#firmware-check').addEventListener('click', () => runCheck(false));
function markFace() { document.querySelectorAll('.face').forEach(b => { const on = b.dataset.face === currentFace; b.classList.toggle('current', on); b.setAttribute('aria-pressed', on); }); }
document.querySelector('#faces').addEventListener('click', event => {
  const button = event.target.closest('.face'); if (!button) return;
  const name = button.querySelector('b').textContent;
  if (button.dataset.face === currentFace) { message('This clock is already showing ' + name + '.', true); return; }
  startUpdate('Switch to the ' + name + ' clock face? Your clock will restart. Unsaved settings will be discarded.', button.dataset.face);
});
refreshFirmware();

function hideWelcome() { document.querySelector('#welcome')?.remove(); document.querySelector('.sidebar nav a[href="#welcome"]')?.remove(); }
document.querySelector('#welcome-dismiss')?.addEventListener('click', async () => {
  try { const response = await fetch('/api/welcome/dismiss', { method: 'POST' }); if (response.ok) hideWelcome(); } catch (_) {}
});
document.querySelector('#welcome-start-button')?.addEventListener('click', async event => {
  const button = event.currentTarget, note = document.querySelector('#welcome-start-note');
  button.disabled = true;
  try {
    const response = await fetch('/api/welcome/start', { method: 'POST' });
    note.textContent = response.ok ? 'Restarting… this page reloads in a few seconds.' : 'That did not work. Try again.';
    if (response.ok) setTimeout(() => location.reload(), 8000); else button.disabled = false;
  } catch (_) { note.textContent = 'That did not work. Try again.'; button.disabled = false; }
});

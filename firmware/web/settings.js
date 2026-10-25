'use strict';
const form = document.querySelector('#settings-form');
const controls = document.querySelector('#controls');
const save = document.querySelector('#save');
const saveBar = document.querySelector('.save-bar');
const notice = document.querySelector('#notice');
let invaders = false, pongSlide = false;  // the Invaders face: a tap on the controller fires
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
  const nightOn = field('nightEnabled').checked, clockOn = field('nightClock').checked;
  document.querySelector('#night-fields').hidden = !clockOn || (!nightOn && !ownNight);
  document.querySelector('#night-schedule').hidden = ownNight || !clockOn;
  document.querySelector('#night-sun').hidden = ownNight || !clockOn;
  document.querySelector('#own-night').hidden = !ownNight || !clockOn;
  document.querySelector('#night-times').hidden = field('nightSun').checked || ownNight;
  const sunHint = document.querySelector('#sun-hint'); sunHint.hidden = ownNight || !field('nightSun').checked || !sunHint.textContent;
  document.querySelector('#range-low').textContent = auto ? 'Dimmest' : 'Minimum';
}
let lightNow = -1, nightWhy = '';
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
  document.querySelector('#sensor-reading').textContent = lightNow >= 0 ? 'Current sensor reading: ' + lightNow + (nightWhy ? ' · Night clock on: ' + (nightWhy === 'dark' ? 'the reading is below the dark value' : 'the night schedule says so') : '') : 'Sensor is off.';
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
    initial = sent; networkAddress(result.network); message('Saved'); noticeTimer = setTimeout(() => { notice.hidden = true; }, 2000);
    if (payload(false).toString() !== initial) saveTimer = setTimeout(autosave, 300);
  } catch (error) { message(error.message, true); }
  saving = false;
}
function networkAddress(network) {
  if (!network) return;
  const url = network.hostname ? 'http://' + network.hostname + '/' : '';
  for (const id of ['#network-host', '#welcome-name']) {
    const link = document.querySelector(id);
    if (link) { link.textContent = url || '—'; link.href = url || '#'; }
  }
  // Keep this page reachable when its old .local address stops being advertised.
  if (network.ip && network.hostname && location.hostname.endsWith('.local') && location.hostname !== network.hostname)
    location.replace('http://' + network.ip + location.pathname + location.search + location.hash);
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
// The sections are shown, hidden and filled after the settings arrive, so the browser's own jump to #network lands before the layout is final: jump once more when it is.
let jumped = false;
function jumpToHash() {
  if (jumped || !location.hash) return;
  jumped = true;
  const target = document.getElementById(decodeURIComponent(location.hash.slice(1)));
  if (target && !target.hidden) requestAnimationFrame(() => target.scrollIntoView());
}
async function load() {
  try {
    const response = await fetch('/api/settings', { cache: 'no-store' });
    if (!response.ok) throw new Error('The clock could not load its settings. Reload this page to try again.');
    const data = await response.json();
    if (!window.wledStarted) { window.wledStarted = true; initWled(); }
    document.querySelector('#nvs-banner').hidden = !(data.nvsFree < 40);
    showPin(data.pinSet);
    const racing = data.mode === 'F1' || data.mode === 'MotoGP', sporty = racing || data.mode === 'Football';
    const pacman = data.mode === 'Pac-Man', arcade = pacman || data.mode === 'Invaders' || data.mode === 'Pong';   // faces you can play from this page
    field('dither').closest('label').style.display = arcade || data.mode === 'Mario' || data.mode === 'Luigi' ? 'none' : '';  // those faces decide for themselves
    const homeFace = data.mode === 'Home'; document.querySelector('#homeassistant').hidden = !homeFace; document.querySelector('#nav-ha').hidden = !homeFace;
    if (homeFace) { initHomeAssistant(); document.querySelector('.sidebar nav a.active')?.classList.remove('active'); document.querySelector('#nav-ha').classList.add('active'); }
    section('[data-sports]', sporty || data.mode === 'Words' || data.mode === 'Mario' || data.mode === 'Luigi' || arcade); section('[data-sportfields]', sporty); section('[data-word]', data.mode === 'Words'); section('[data-mario]', data.mode === 'Mario' || data.mode === 'Luigi'); section('[data-pacman]', arcade);
    section('[data-football]', data.mode === 'Football'); section('[data-f1]', racing);
    if (sporty || data.mode === 'Words' || data.mode === 'Mario' || data.mode === 'Luigi' || arcade) { document.querySelector('.sidebar nav a.active')?.classList.remove('active'); document.querySelector('.sidebar nav a[href="#mode"]').classList.add('active'); }
    invaders = data.mode === 'Invaders'; pongSlide = data.mode === 'Pong';
    document.querySelector('#play').hidden = !arcade;
    if (arcade && !pacman) {   // no colours and no Bluetooth controller (yet) on these faces, and their own instructions
      for (const node of document.querySelectorAll('[data-pacman] > :not(#play):not(#pong-opts):not(#pong-speed):not(#pong-level):not(#pong-date):not(#pong-day):not(#arc-colors):not(#arc-play-h):not(#arc-clock-h), #pad-status, #pad-power')) node.hidden = true;
      document.querySelector('#pad-power').closest('.row').hidden = true;
      document.querySelector('#pong-opts').hidden = data.mode !== 'Pong'; document.querySelector('#pong-speed').hidden = data.mode !== 'Pong'; document.querySelector('#pong-level').hidden = data.mode !== 'Pong';
      document.querySelector('#play > .hint').textContent = data.mode === 'Pong'
        ? 'Open the full-screen controller (a joystick appears wherever you touch), use the arrow keys or W S, or use a game controller. Turn the dial like the original game, by swiping up and down anywhere on the screen: your paddle (the left, orange one) follows it and stops when you stop. With the keys, up and down move it and left or right stops it. A match goes to 7 points and the score on top is yours; then, and after 20 seconds without a move, the computer players take over again.'
        : 'Open the full-screen controller: swipe left and right anywhere to move the cannon, and every touch fires, so a second finger can fire while the first one steers. The arrow keys or A D move, the space bar fires, and so does a game controller. The clock plays again after 20 seconds without a move.';
    }
    for (const a of document.querySelectorAll('.sidebar nav a[href="/wled"], .sidebar nav a[href="/canvas"], .sidebar nav a[href="/gifs"]')) a.hidden = !!data.btPad;   // off while the Bluetooth controller is on
    if (pacman) field('clockShade').closest('.field').hidden = true;   // this face has no background clock to dim
    if (arcade && !pacman) {   // Invaders and Pong: the clock and the score
      document.querySelector('#arc-colors').hidden = false; document.querySelector('#pong-date').hidden = data.mode !== 'Pong'; document.querySelector('#pong-day').hidden = data.mode !== 'Pong';
      { const score = field('arcScore').closest('.field'), p = data.mode === 'Pong'; score.hidden = p; score.style.display = p ? 'none' : ''; score.querySelector('select').disabled = p; field('arcHours').closest('.field').querySelector('label').textContent = p ? 'Hours colour (left player)' : 'Clock hours colour'; field('arcMinutes').closest('.field').querySelector('label').textContent = p ? 'Minutes colour (right player)' : 'Clock minutes colour'; } document.querySelector('#arc-play-h').hidden = false; document.querySelector('#arc-clock-h').hidden = false;
      for (const name of ['arcHours', 'arcMinutes', 'arcScore']) field(name).replaceChildren(...['Default', 'Pastel green', 'Pastel pink', 'Pastel blue', 'Lemon', 'Peach', 'Pastel purple', 'Red', 'Orange', 'Cyan', 'White'].map((label, value) => new Option(label, value)));
    }
    if (pacman) {
      const colours = ['Default', 'Pastel green', 'Pastel pink', 'Pastel blue', 'Lemon', 'Peach', 'Pastel purple', 'Red', 'Orange', 'Cyan', 'White'];
      for (const name of ['ghost1Color', 'ghost2Color', 'ghost3Color', 'ghost4Color', 'dotColor', 'pacmanClockColor', 'clockMinutesColor']) field(name).replaceChildren(...colours.map((label, value) => new Option(label, value)));
    }
    if (data.mode === 'MotoGP') document.querySelector('[data-f1] .field > label').textContent = 'Favourite riders';
    if (racing) form.querySelector('[name=timeStyle] option[value="0"]').remove();
    { const lv = field('pongLevel'), out = document.querySelector('#pong-level-out'); if (lv && !lv.dataset.wired) { lv.dataset.wired = '1'; lv.addEventListener('input', () => { out.textContent = +lv.value + 1; }); } }
    Object.entries(data.settings).forEach(([name, value]) => { const input = field(name); if (!input) return; if (input.tagName === 'SELECT' && ![...input.options].some(o => o.value === String(value))) input.add(new Option(value, value)); if (input.type === 'checkbox') input.checked = Boolean(value); else input.value = name === 'use24h' ? (value ? '1' : '0') : name === 'timeStyle' && racing && Number(value) === 0 ? 2 : value; });
    { const lv = field('pongLevel'); if (lv) document.querySelector('#pong-level-out').textContent = +lv.value + 1; }
    loadBirthdays(field('birthdays').value);
    document.querySelector('#sun-hint').textContent = data.sunTimes || '';
    document.querySelector('#mode-name').textContent = data.mode; currentFace = data.modeId; markFace();
    document.querySelector('#sport-heading').textContent = data.mode + ' preferences';
    networkAddress(data.network);
    document.querySelector('#network-name').textContent = data.network.ssid || 'Not connected';
    document.querySelector('#network-ip').textContent = data.network.ip || '—';
    document.querySelector('#sidebar-ip').textContent = data.network.ip || 'Offline';
    if (data.welcomeHidden && !data.welcomeMode) hideWelcome();
    document.body.classList.toggle('welcome-only', !!data.welcomeMode);
    { const start = document.querySelector('#welcome-start'); if (start) start.hidden = !data.welcomeMode; const dismiss = document.querySelector('#welcome-dismiss'); if (dismiss) dismiss.parentElement.hidden = !!data.welcomeMode; }
    { const link = document.querySelector('#welcome-ip'); const url = data.network.ip ? 'http://' + data.network.ip + '/' : ''; if (link) { link.textContent = url || '—'; link.href = url || '#'; } }
    lightNow = data.lightReading; nightWhy = data.nightWhy || '';
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
    jumpToHash();
  } catch (error) { message(error.message, true); document.querySelector('#save-status').textContent = 'Settings could not load'; saveBar.hidden = false; }
}
form.addEventListener('input', event => { brightness(event); sensor(event); timezoneMode(); if (!/^(text|search|url)$/.test(event.target.type)) dirty(); });  // typed text saves on change (blur)
for (const [id, name] of [['#set-dark', 'ldrDark'], ['#set-light', 'ldrLight']]) {
  document.querySelector(id).addEventListener('click', () => { if (lightNow < 0) return; field(name).value = lightNow; sensor({ target: field(name) }); dirty(); });
}
let networkPollAt = 0;
setInterval(async () => {
  if (document.hidden || controls.disabled) return;
  if (!field('autoBrightness').checked && Date.now() - networkPollAt < 10000) return;
  networkPollAt = Date.now();
  try { const data = await (await fetch('/api/settings', { cache: 'no-store' })).json(); networkAddress(data.network); lightNow = data.lightReading; nightWhy = data.nightWhy || ''; sensor(); } catch (error) { /* next poll retries */ }
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
    const bar = document.querySelector('#update-banner-progress'); bar.hidden = !state.busy || !state.total; bar.value = state.total ? state.received / state.total * 100 : 0;
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

// Colour quality: the clock lowers it by itself when memory runs short (and tries again after a quiet day when it was a busy time)
async function refreshDisplay() {
  try {
    const response = await fetch('/api/display', {cache:'no-store'}); if (!response.ok) return;
    const state = await response.json(), select = document.querySelector('#display-quality');
    select.innerHTML = '';
    select.add(new Option('Automatic', '0'));
    state.levels.forEach((name, index) => select.add(new Option((index === 0 ? 'Best' : index === state.levels.length - 1 ? 'Lowest' : 'Level ' + (index + 1)) + ' · ' + name, String(index + 1))));
    select.value = String(state.lock);
    const name = state.levels[state.level - 1];
    let text = 'Running at ' + name + '.';
    if (state.lock) text = 'Fixed at ' + name + '. The clock does not change it.';
    else if (state.level > 1) text = 'Lowered to ' + name + ' after memory ran short' + (state.reason === 'busy' ? ' during live data. It tries the best quality again from ' + state.retryHour + ':00, when nothing live is running.' : ' at a quiet time. It stays until you reset it.');
    document.querySelector('#display-status').textContent = text;
    document.querySelector('#display-reset').hidden = state.lock !== 0 || state.level === 1;
    const lowered = state.lock === 0 && state.level > 1;
    document.querySelector('#display-banner').hidden = !lowered;
    if (lowered) document.querySelector('#display-banner-text').textContent = 'The clock ran short of memory and now runs at ' + name + ' (level ' + state.level + ' of ' + state.levels.length + '). ' + (state.reason === 'busy' ? 'It tries the best quality again from ' + state.retryHour + ':00 when nothing live is running.' : 'It stays until you reset it.');
  } catch (_) {}
}
async function saveDisplay(body) {
  try {
    const response = await fetch('/api/display', {method:'POST', body}); const result = await response.json();
    if (!response.ok) throw new Error(result.message);
    busy = true; controls.disabled = true; message(result.message); setTimeout(reconnect, 2500);
  } catch (error) { message(error.message || 'Could not save the display quality.', true); }
}
document.querySelector('#display-quality').addEventListener('change', event => saveDisplay(new URLSearchParams({lock: event.target.value})));
document.querySelector('#display-reset').addEventListener('click', () => saveDisplay(new URLSearchParams({reset: '1'})));
document.querySelector('#display-banner-reset').addEventListener('click', () => saveDisplay(new URLSearchParams({reset: '1'})));
refreshDisplay();

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

// Home Assistant (Home face): its own card and save button, outside the autosaving form so the token is only sent when typed
const HA_TILES = ['Temperature', 'Humidity', 'People home', 'Power', 'Custom number', 'CO₂ (ppm)', 'Rain', 'EV charge (%)', 'Energy price', 'Outdoor humidity', 'Weather', 'Off'], HA_TILE_AUTO = [true, true, true, false, false, false, false, false, true, true, true, false];
const HA_ICONS = ['Door', 'Window', 'Garage door', 'Washer', 'Dryer', 'Lights', 'Unlocked locks', 'Charger', 'Off', 'Occupied rooms', 'Low batteries', 'Offline devices', 'Water leaks', 'Blinds open', 'Media playing', 'Heating'], HA_ICON_AUTO = [true, true, true, false, false, true, true, true, false, true, true, true, true, true, true, true];
const HA_TILE_KIND = ['temperature', 'humidity', 'person', 'power', 'sensor', 'co2', 'rain', 'battery', 'price', 'humidity', 'weather', 'any'], HA_ICON_KIND = ['door', 'window', 'garage', 'any', 'any', 'light', 'lock', 'any', 'any', 'presence', 'battery_status', 'any', 'leak', 'blinds', 'media', 'climate'];
// What Auto detects for each type, shown as grey tags while the entity field is empty
const HA_TILE_DETECT = {0: 'temperature', 1: 'humidity', 2: 'person', 9: 'weather', 10: 'weather'}, HA_TILE_NOTE = {8: 'The tariff of the Energy dashboard’s grid source'};
const HA_ICON_DETECT = {0: 'door', 1: 'window', 2: 'garage', 5: 'light', 6: 'lock', 7: 'charging', 9: 'presence', 10: 'battery_status', 12: 'leak', 13: 'blinds', 14: 'media', 15: 'climate'}, HA_ICON_NOTE = {11: 'Every device Home Assistant reports as unavailable'};
const HA_CAR = /car|ev_|_ev|tesla|vehicle|charger|wallbox/;
const HA_ENERGY_KIND = ['power', 'power', 'power', 'battery', 'power', 'energy', 'energy', 'energy', 'energy'];
const HA_ENERGY = ['House power now', 'Solar power now', 'Battery power now', 'Battery charge (%)', 'Grid power now', 'House energy today', 'Solar energy today', 'Battery energy today', 'Grid energy today'];
let haStarted = false, haTimer = 0;
const haEl = (tag, attrs = {}, ...kids) => { const node = Object.assign(document.createElement(tag), attrs); node.append(...kids); return node; };
function haInput(value, placeholder, extra = {}) { return haEl('input', Object.assign({type: 'text', value: value || '', placeholder, autocomplete: 'off', spellcheck: false}, extra)); }
function haSelect(names, value) { const select = haEl('select'); names.forEach((name, index) => select.add(new Option(name, index))); select.value = value; return select; }
function haGroup(title, hint) { return haEl('div', {className: 'ha-group'}, haEl('h3', {textContent: title}), haEl('p', {className: 'hint', textContent: hint})); }
function haCheck(label, checked) { const box = haEl('input', {type: 'checkbox', checked}); return [box, haEl('label', {className: 'ha-auto'}, box, label)]; }
// Autocomplete from Home Assistant: the clock asks it for matching entities of the right kind while you type
let haList = null, haSuggestTimer = 0, haSuggestSeq = 0;
// The entities of each kind are fetched once and kept in this browser (localStorage), then filtered here while typing
const haCache = {};
async function haEntities(kind) {
  if (haCache[kind]) return haCache[kind];
  try { const kept = JSON.parse(localStorage.getItem('ha-' + kind)); if (kept && Date.now() - kept.at < 36e5 && kept.list.length) return (haCache[kind] = kept.list); } catch (_) {}
  const answer = await (await fetch('/api/ha/suggest?kind=' + encodeURIComponent(kind) + '&q=', {cache: 'no-store'})).json();
  if (answer.entities.length) { haCache[kind] = answer.entities; try { localStorage.setItem('ha-' + kind, JSON.stringify({at: Date.now(), list: answer.entities})); } catch (_) {} }
  return answer.entities;
}
function haBindSuggest(input, kind) {
  input.setAttribute('list', 'ha-suggest');
  const ask = () => {
    clearTimeout(haSuggestTimer);
    haSuggestTimer = setTimeout(async () => {
      const seq = ++haSuggestSeq;
      try {
        const typed = input.value.trim().toLowerCase();
        let list = (await haEntities(kind())).filter(item => !typed || item.id.includes(typed) || item.name.toLowerCase().includes(typed));
        if (seq !== haSuggestSeq) return;
        haList.replaceChildren(...list.slice(0, 40).map(item => { const option = document.createElement('option'); option.value = item.id; option.label = item.name + (item.unit ? ' (' + item.unit + ')' : ''); return option; }));
      } catch (_) {}
    }, 150);
  };
  input.addEventListener('focus', ask);
  input.addEventListener('input', ask);
}
// Several entities in one field, as tags (like the favourite teams): the value stays "id,id,id", with "auto" as a single tag
const HA_TILE_MULTI = [0, 1, 2, 3];  // temperature, humidity, people and power tiles combine several entities
function haLock(root, off) { root.querySelector('input').disabled = off; root.style.opacity = off ? 0.5 : ''; root.style.pointerEvents = off ? 'none' : ''; }
function haTags(value, placeholder, kind, limit = 95) {
  let ids = (value || '').split(',').map(id => id.trim()).filter(Boolean);
  const list = haEl('div', {className: 'tags'}), input = haEl('input', {type: 'text', placeholder, autocomplete: 'off', spellcheck: false});
  const box = haEl('div', {className: 'tag-box'}, list, input), menu = haEl('div', {className: 'suggest', hidden: true}), note = haEl('p', {className: 'hint'});
  const root = haEl('div', {}, box, menu, note);
  let shown = [], active = 0, seq = 0;
  const changed = () => { render(); root.dispatchEvent(new Event('change', {bubbles: true})); };
  const render = () => {
    list.replaceChildren(...ids.map(id => {
      const remove = haEl('button', {type: 'button', className: 'tag-x', textContent: '×'}); remove.setAttribute('aria-label', 'Remove ' + id);
      remove.addEventListener('click', () => { ids = ids.filter(one => one !== id); changed(); input.focus(); });
      return haEl('span', {className: 'tag'}, id, remove);
    }));
    note.textContent = ids.join(',').length > limit ? 'Too many for the clock to store: remove one.' : '';
    note.hidden = !note.textContent;
  };
  const add = id => {
    id = id.trim().toLowerCase();
    if (!/^[a-z_]+\.[a-z0-9_]+$/.test(id) || ids.includes(id)) return false;
    ids = ids.filter(one => one !== 'auto' && one !== 'off' && one[0] !== '-'); ids.push(id); input.value = ''; menu.hidden = true; changed(); return true;
  };
  const suggest = async () => {
    const mine = ++seq, typed = input.value.trim().toLowerCase();
    try {
      const all = await haEntities(kind());
      if (mine !== seq) return;
      shown = all.filter(item => !ids.includes(item.id) && (!typed || item.id.includes(typed) || item.name.toLowerCase().includes(typed))).slice(0, 8); active = 0;
      menu.replaceChildren(...shown.map((item, i) => {
        const row = haEl('div', {className: 'suggest-item' + (i === active ? ' active' : ''), textContent: item.name + ' · ' + item.id + (item.unit ? ' (' + item.unit + ')' : '')});
        row.addEventListener('mousedown', event => { event.preventDefault(); add(item.id); input.focus(); });
        return row;
      }));
      menu.hidden = !shown.length;
    } catch (_) { menu.hidden = true; }
  };
  const mark = () => [...menu.children].forEach((row, i) => row.classList.toggle('active', i === active));
  input.addEventListener('focus', suggest); input.addEventListener('input', suggest);
  input.addEventListener('blur', () => { if (input.value.trim()) add(input.value); menu.hidden = true; });
  input.addEventListener('keydown', event => {
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') { if (shown.length) { event.preventDefault(); active = (active + (event.key === 'ArrowDown' ? 1 : shown.length - 1)) % shown.length; mark(); } }
    else if (event.key === 'Enter' || event.key === ',') { event.preventDefault(); if (!menu.hidden && shown[active] && !/^[a-z_]+\.[a-z0-9_]+$/.test(input.value.trim().toLowerCase())) add(shown[active].id); else add(input.value); }
    else if (event.key === 'Escape') menu.hidden = true;
    else if (event.key === 'Backspace' && !input.value && ids.length) { ids.pop(); changed(); }
  });
  box.addEventListener('click', () => input.focus());
  Object.defineProperty(root, 'value', {get: () => ids.join(','), set: text => { ids = (text || '').split(',').map(id => id.trim()).filter(Boolean); render(); }});
  render();
  return root;
}
// An empty entity field means Auto: the entities found in Home Assistant show as grey tags (refresh detects again). Removing one of them, or typing your own entity, turns it into your own list.
function haAutoRow(type, autoBox, entity, off, can, detect, notes, allowNone = false, exclude = false) {
  const preview = haEl('div', {className: 'ha-detected'}), refresh = haEl('button', {type: 'button', className: 'ha-refresh', title: 'Detect again', textContent: '↻'}), field = entity.querySelector('input');
  let seq = 0;
  const parts = () => entity.value.split(',').map(t => t.trim()).filter(Boolean);  // exclude: "-id" parts are detected ones left out
  const show = async fresh => {
    const mine = ++seq, t = type.value;
    preview.hidden = !autoBox.checked;
    if (!autoBox.checked) return;
    if (notes[t]) { preview.textContent = notes[t]; return; }
    preview.textContent = 'Detecting…';
    try {
      const kind = detect[t] === 'ev_battery' ? 'battery' : detect[t];
      if (fresh) { delete haCache[kind]; try { localStorage.removeItem('ha-' + kind); } catch (_) {} }
      let list = await haEntities(kind);
      if (detect[t] === 'charging' || detect[t] === 'ev_battery') list = list.filter(item => HA_CAR.test(item.id));
      if (mine !== seq) return;
      const left = exclude ? parts().filter(t => t[0] === '-').map(t => t.slice(1)) : [];
      const ids = list.map(item => item.id).filter(id => !left.includes(id));
      if (!ids.length) { preview.textContent = 'Nothing found in Home Assistant.'; return; }
      preview.replaceChildren(...ids.slice(0, 12).map(id => {
        const tag = haEl('span', {className: 'tag tag-auto'}, id);
        {
          const x = haEl('button', {type: 'button', className: 'tag-x', textContent: '×'}); x.setAttribute('aria-label', 'Remove ' + id);
          x.addEventListener('click', () => { entity.value = exclude ? [...parts(), '-' + id].join(',') : ids.filter(one => one !== id).join(',') || (allowNone ? 'off' : ''); entity.dispatchEvent(new Event('change', {bubbles: true})); });
          tag.append(x);
        }
        return tag;
      }), ...(ids.length > 12 ? [haEl('span', {className: 'hint', textContent: '+' + (ids.length - 12) + ' more'})] : []));
    } catch (_) { preview.textContent = 'Could not ask Home Assistant.'; }
  };
  const sync = () => {
    const ok = !!can[type.value];
    autoBox.checked = ok && (exclude ? parts().every(t => t[0] === '-') : !entity.value.trim());
    refresh.style.visibility = ok ? '' : 'hidden';
    haLock(entity, off(type.value));
    field.placeholder = autoBox.checked ? (exclude && entity.value.trim() ? 'Using what was detected, except the tags here. Type to choose your own…' : 'Using what was detected. Type to choose your own…') : 'Type to find an entity…';
    show(false);
  };
  type.addEventListener('change', sync); entity.addEventListener('change', sync);
  refresh.addEventListener('click', () => show(true));
  return {refresh, preview, sync};
}
// The three entities of the EV tile as "charge,charging,plugged": "-" for one left to detection, "off" for one that is not used, nothing at all when all three are
function haEvEntity(ev) { const ids = ev.map(e => e.field.value.split(',')[0] || '-'); /* one entity per part */ while (ids.length && ids[ids.length - 1] === '-') ids.pop(); return ids.join(','); }
// WLED overlay: switch and stream status
async function initWled() {
  const $ = (id) => document.querySelector('#wled-' + id);
  const show = async () => {
    try {
      const o = await (await fetch('/api/wled', { cache: 'no-store' })).json();
      $('enabled').checked = o.enabled; $('ignore').checked = o.ignoreLive; if (document.activeElement !== $('timeout')) $('timeout').value = o.timeoutMin;
      $('status').textContent = !o.enabled ? 'Off.' : !o.listening ? 'On, but the pixel stream listeners are closed (not enough free memory right now).'
        : o.live ? 'Receiving pixels over ' + o.source + '.' : o.on ? 'Showing an effect.' : 'On: showing the clock until content arrives. ' + o.packets + ' packets received.';
    } catch (_) {}
  };
  const save = async () => {
    const body = new URLSearchParams({ enabled: $('enabled').checked ? '1' : '0', timeoutMin: $('timeout').value || '0', ignoreLive: $('ignore').checked ? '1' : '0' });
    try { const r = await fetch('/api/wled', { method: 'POST', body }); if (!r.ok) $('status').textContent = (await r.json()).message; else show(); } catch (_) {}
  };
  $('enabled').addEventListener('change', save); $('timeout').addEventListener('change', save); $('ignore').addEventListener('change', save);
  show(); setInterval(show, 5000);
}

async function initHomeAssistant() {
  if (haStarted) return;
  haStarted = true;
  // The card sits among the settings, but saves by itself: keep its fields out of the form's autosave and its Enter key from submitting
  const card = document.querySelector('#homeassistant');
  for (const type of ['input', 'change']) card.addEventListener(type, event => event.stopPropagation());
  card.addEventListener('keydown', event => { if (event.key === 'Enter' && event.target.tagName === 'INPUT') event.preventDefault(); });
  const state = await (await fetch('/api/ha', {cache: 'no-store'})).json();
  const body = document.querySelector('#ha-body');
  haList = haEl('datalist', {id: 'ha-suggest'}); body.append(haList);
  const host = haInput(state.host, 'homeassistant.local'), port = haEl('input', {type: 'number', min: 1, max: 65535, value: state.port});
  const token = haEl('input', {type: 'password', placeholder: state.tokenSet ? 'Saved. Type to replace it.' : 'Long-lived access token', autocomplete: 'off'});
  const [tlsBox, tlsLabel] = haCheck('Use a secure connection (https)', state.tls);
  body.append(haEl('div', {className: 'ha-two'}, haEl('div', {}, haEl('label', {textContent: 'Address'}), host), haEl('div', {}, haEl('label', {textContent: 'Port'}), port)),
    haEl('p', {className: 'hint', textContent: 'Name or IP address of Home Assistant. The port is 8123 on its own, or 443 behind a reverse proxy with https.'}),
    haEl('div', {className: 'field'}, haEl('label', {textContent: 'Access token'}), token, haEl('p', {className: 'hint', textContent: 'Create one in Home Assistant under your profile, Security, Long-lived access tokens. The clock keeps it and never shows it again.'})), tlsLabel);
  // A pasted address (https://name:port) fills the other fields; switching https moves the port between its two usual values
  const readAddress = () => {
    let text = host.value.trim();
    if (/^https:\/\//i.test(text)) { tlsBox.checked = true; if (!/:\d+(\/|$)/.test(text.replace(/^https:\/\//i, ''))) port.value = 443; }
    else if (/^http:\/\//i.test(text)) { tlsBox.checked = false; if (!/:\d+(\/|$)/.test(text.replace(/^http:\/\//i, ''))) port.value = 8123; }
    text = text.replace(/^https?:\/\//i, '').replace(/\/.*$/, '');
    const colon = text.lastIndexOf(':');
    if (colon > 0 && /^\d+$/.test(text.slice(colon + 1))) { port.value = text.slice(colon + 1); text = text.slice(0, colon); }
    host.value = text;
  };
  host.addEventListener('change', readAddress);
  tlsBox.addEventListener('change', () => { if (tlsBox.checked && ['8123', '80'].includes(port.value)) port.value = 443; else if (!tlsBox.checked && port.value === '443') port.value = 8123; });

  const [energyBox, energyLabel] = haCheck('Energy page', state.pages & 2), [homeBox, homeLabel] = haCheck('Home page', state.pages & 1);
  const [mediaBox, mediaLabel] = haCheck('Now playing page', state.pages & 4), [devicesBox, devicesLabel] = haCheck('Device problems page', state.pages & 8), [backdropBox, backdropLabel] = haCheck('Show the album art, dimmed, behind every page', state.pages & 16);
  const secs = haEl('input', {type: 'number', min: 3, max: 120, value: state.pageSecs});
  const pages = haGroup('Pages', 'The clock starts on the Energy page and rotates between the pages you tick. A page without data is skipped: the Energy page needs its entities below, Now playing needs something playing, and Device problems lists low batteries (20% or less) and offline devices, so it only shows while there are any. The album art option needs a media player under Now playing; while it plays, its cover sits at a quarter brightness behind every other page.');
  pages.append(haEl('div', {className: 'ha-two ha-three'}, energyLabel, homeLabel, mediaLabel, devicesLabel), haEl('div', {className: 'field'}, backdropLabel), haEl('div', {className: 'field'}, haEl('label', {textContent: 'Seconds per page'}), secs));
  body.append(pages);
  const playing = haGroup('Now playing', 'Pick up to four rooms (media players). The page shows the title, the artist and the album art of the first of them that is playing (the order below), and is skipped when none is. Album art needs a JPEG picture from Home Assistant; other pictures show a note.');
  const rooms = haTags((state.media || []).filter(Boolean).join(','), 'Type to find a media player…', () => 'media', 400);
  playing.append(haEl('div', {className: 'field'}, haEl('label', {textContent: 'Rooms'}), rooms));
  body.append(playing);

  const rows = {tiles: [], icons: []};
  const tiles = haGroup('Tiles (up to six)', 'Choose each visible reading; set a tile to Off to hide it. The tiles that are on spread evenly over the panel, in this order, left to right. EV charge, CO₂, rain and price need a sensor entity. Rain and price show the sensor’s value without a unit suffix or conversion. While a field is empty the clock uses everything it detects in Home Assistant (grey tags; ↻ detects again): outdoor humidity reads the weather entity, price the tariff of the Energy dashboard’s grid source, and temperature and humidity are averaged. To use your own instead, type an entity or remove a grey tag; temperature, humidity, people and power tiles accept several (averaged, or added up for power).');
  state.tiles.forEach(slot => {
    const type = haSelect(HA_TILES, slot.type), autoBox = {checked: slot.auto}, entity = haTags(slot.type === 7 ? '' : slot.entity, 'Type to find an entity…', () => HA_TILE_KIND[type.value] || 'sensor'), unit = haInput(slot.unit, 'unit', {maxLength: 3});
    const auto = haAutoRow(type, autoBox, entity, value => value === '11', HA_TILE_AUTO, HA_TILE_DETECT, HA_TILE_NOTE);
    // EV charge is one tile with three entities: the charge, whether it charges and whether it is plugged in (an empty one is detected)
    const parts = (slot.type === 7 ? slot.entity : '').split(',').map(id => id.trim() === '-' ? '' : id.trim()), evKind = ['battery', 'charging', 'plug'], evName = ['Charge', 'Charging', 'Plugged in'], evDetect = ['ev_battery', 'charging', 'plug'];
    const ev = evName.map((name, k) => { const field = haTags(parts[k] || '', 'Type to find an entity…', () => evKind[k]), found = haAutoRow(type, {checked: false}, field, () => false, {7: true}, {7: evDetect[k]}, {}, true); return {field, found, name}; });
    const evBlock = haEl('div', {className: 'ha-ev'}, ...ev.map(e => haEl('div', {className: 'ha-ev-row'}, haEl('span', {className: 'hint', textContent: e.name, title: 'One entity. Remove the others from the detected list to choose.'}), e.found.refresh, haEl('div', {}, e.field, e.found.preview))));
    const sync = () => { const isEv = type.value === '7'; if (!isEv && !HA_TILE_MULTI.includes(+type.value) && entity.value.includes(',')) entity.value = entity.value.split(',')[0]; unit.hidden = type.value !== '4'; entity.style.display = isEv ? 'none' : ''; auto.preview.style.display = isEv ? 'none' : ''; evBlock.hidden = !isEv; };
    type.addEventListener('change', sync); sync(); auto.sync(); ev.forEach(e => e.found.sync());
    tiles.append(haEl('div', {className: 'ha-row'}, type, auto.refresh, haEl('div', {}, entity, auto.preview, evBlock), unit));
    rows.tiles.push({type, autoBox, entity, unit, ev});
  });
  body.append(tiles);

  const icons = haGroup('State icons', 'Low batteries counts devices at 20% or less (or reporting low battery). Offline counts devices with unavailable entities; entities without a device count separately. Heating uses actual heating activity. Select a group to limit what is counted. Lights with your own entities counts rooms with those lights on, not individual bulbs (lights without an area are left out); while the field is empty it counts every light. An empty field uses everything detected (grey tags; ↻ detects again); type an entity or remove a grey tag to use your own. Occupied rooms counts distinct Home Assistant areas with active motion or occupancy sensors; sensors without an area are left out. Lit when something of that kind is open, running or on. Alert after: flash a message once it has been lit that many minutes (doors, windows, garage), 0 for never.');
  state.icons.forEach(slot => {
    const type = haSelect(HA_ICONS, slot.type), autoBox = {checked: slot.auto}, entity = haTags(slot.entity, 'Type to find an entity…', () => HA_ICON_KIND[type.value] || 'any', 1023), alert = haEl('input', {type: 'number', min: 0, max: 240, value: slot.alertMins, title: 'Alert after (minutes)'});
    const auto = haAutoRow(type, autoBox, entity, value => value === '8', HA_ICON_AUTO, HA_ICON_DETECT, HA_ICON_NOTE, false, true);
    const sync = () => { alert.hidden = !['0', '1', '2'].includes(type.value); };
    type.addEventListener('change', sync); sync(); auto.sync();
    icons.append(haEl('div', {className: 'ha-row'}, type, auto.refresh, haEl('div', {}, entity, auto.preview), alert));
    rows.icons.push({type, autoBox, entity, alert});
  });
  body.append(icons);

  const energy = haGroup('Energy page', 'Your power and energy sensors (watts or kilowatts are both fine; energy meters that count for ever are fine too: the clock shows their change since midnight). Start typing to get suggestions from Home Assistant; add several sensors to a row (for example two meters) and the clock adds them up. Leave a row empty to hide it. The middle row shows solar, or the battery while the battery powers the house and there is no solar.');
  const energyFields = [];
  const energyInputs = state.energy.map((id, index) => { const input = haTags(id, 'Type to find ' + (HA_ENERGY_KIND[index] === 'battery' ? 'a battery' : 'a ' + HA_ENERGY_KIND[index]) + ' sensor…', () => HA_ENERGY_KIND[index]); const field = haEl('div', {className: 'field'}, haEl('label', {textContent: HA_ENERGY[index]}), input); energyFields.push(field); energy.append(field); return input; });
  const detectNote = haEl('p', {className: 'hint', textContent: ''});
  const detectButton = haEl('button', {type: 'button', className: 'secondary', textContent: 'Fill from Home Assistant'});
  // The sensors of Home Assistant's own Energy dashboard: empty fields are filled, anything already typed stays
  const detect = async quiet => {
    detectButton.disabled = true; detectNote.textContent = 'Asking Home Assistant…';
    try {
      const ask = await fetch('/api/ha/detect', {method: 'POST'});
      if (!ask.ok) { detectNote.textContent = (await ask.json()).message; detectButton.disabled = false; return; }
      for (let tries = 0; tries < 14; tries++) {
        await new Promise(resolve => setTimeout(resolve, 700));
        const answer = await (await fetch('/api/ha/detect', {cache: 'no-store'})).json();
        if (answer.state === 'pending') continue;
        if (answer.state === 'ready') {
          let filled = 0;
          answer.entities.forEach((id, index) => { const had = energyInputs[index].value.trim(); if (id && (!had || (id !== had && id.split(',').length > had.split(',').length && had.split(',').every(one => id.split(',').includes(one))))) { energyInputs[index].value = id; filled++; } });  // empty fields, or a field that holds only some of the detected meters
          if (filled) card.dispatchEvent(new Event('change'));
          detectNote.textContent = filled ? 'Filled ' + filled + ' from the Energy dashboard. Check them, add the rest, then save. Clear a field and press the button to refill it.' : 'Nothing new to fill: the Energy dashboard sensors are already entered.';
        } else detectNote.textContent = answer.note || 'Home Assistant did not answer.';
        detectButton.disabled = false; return;
      }
      detectNote.textContent = 'Home Assistant took too long.';
    } catch (_) { detectNote.textContent = 'The clock did not answer.'; }
    detectButton.disabled = false;
  };
  detectButton.addEventListener('click', () => detect(false));
  const fillField = haEl('div', {className: 'field'}, detectButton, detectNote);
  energy.insertBefore(fillField, energy.children[2]);
  // Auto: the clock takes the sensors from the Energy dashboard of Home Assistant itself, every time it connects
  const [autoBox, autoLabel] = haCheck('Auto: use the Energy dashboard of Home Assistant', state.energy.includes('auto'));
  const autoNote = haEl('p', {className: 'hint', textContent: ''});
  const manual = state.energy.map(id => id === 'auto' ? '' : id);
  const showUsed = async () => {
    autoNote.textContent = 'Asking Home Assistant…';
    try {
      const ask = await fetch('/api/ha/detect', {method: 'POST'});
      if (!ask.ok) { autoNote.textContent = (await ask.json()).message; return; }
      for (let tries = 0; tries < 14 && autoBox.checked; tries++) {
        await new Promise(resolve => setTimeout(resolve, 700));
        const answer = await (await fetch('/api/ha/detect', {cache: 'no-store'})).json();
        if (answer.state === 'pending') continue;
        const used = answer.state === 'ready' ? answer.entities.map((id, index) => id ? HA_ENERGY[index] + ': ' + id.split(',').join(', ') : '').filter(Boolean) : [];
        autoNote.textContent = used.length ? 'Using ' + used.join(' · ') : (answer.note || 'Home Assistant did not answer.');
        return;
      }
      autoNote.textContent = 'Home Assistant took too long.';
    } catch (_) { autoNote.textContent = 'The clock did not answer.'; }
  };
  const applyAuto = fromUser => {
    const on = autoBox.checked;
    energyFields.forEach(field => { field.hidden = on; });
    fillField.hidden = on;
    if (on) {
      energyInputs.forEach((input, index) => { if (input.value !== 'auto') manual[index] = input.value; input.value = index === 3 ? '' : 'auto'; });
      showUsed();
    } else {
      energyInputs.forEach((input, index) => { if (input.value === 'auto') input.value = manual[index]; });
      autoNote.textContent = '';
    }
  };
  autoBox.addEventListener('change', () => applyAuto(true));
  energy.insertBefore(haEl('div', {className: 'field'}, autoLabel, autoNote), energy.children[2]);
  applyAuto(false);
  // Fill the autocomplete cache in the background, one kind at a time (the clock answers one request at a time)
  if (state.state === 4) (async () => {
    const kinds = new Set(['any', 'sensor', ...Object.values(HA_TILE_KIND), ...Object.values(HA_ICON_KIND), ...HA_ENERGY_KIND]);
    for (const kind of kinds) { try { await haEntities(kind); } catch (_) {} }
  })();
  if (state.state === 4 && !autoBox.checked && energyInputs.every(input => !input.value.trim())) detect(true);  // a first visit: fill it in by itself
  const [gridBox, gridLabel] = haCheck('Grid power is positive when importing', state.gridImportPositive), [batteryBox, batteryLabel] = haCheck('Battery power is positive when discharging', state.batteryDischargePositive);
  energy.append(haEl('div', {className: 'ha-two'}, gridLabel, batteryLabel));
  body.append(energy);

  const alerts = haGroup('Alerts', 'Alarm alerts need no setup: any alarm panel that triggers shows ALARM. Washer-done alerts work independently of the state icons. Dryer and charger alerts use their state icons.');
  const doorbell = haInput(state.doorbell, 'binary_sensor.doorbell');
  haBindSuggest(doorbell, () => 'binary');
  alerts.append(haEl('div', {className: 'field'}, haEl('label', {textContent: 'Doorbell (turns on when pressed)'}), doorbell));
  const washer = haInput(state.washer?.entity || '', 'binary_sensor.washer_running');
  haBindSuggest(washer, () => 'any');
  alerts.append(haEl('div', {className: 'field'}, haEl('label', {textContent: 'Washer running (popup when it finishes)'}), washer));
  body.append(alerts);

  const popups = haGroup('Popups', 'What each popup does. Also at night: show it through the night clock. Full brightness: light the panel fully while it is up. Flash: flash the whole panel in the popup\'s colour.');
  const addPopup = (label, flags) => {
    const [night, nightLabel] = haCheck('Also at night', flags & 1), [bright, brightLabel] = haCheck('Full brightness', flags & 2), [flash, flashLabel] = haCheck('Flash', flags & 4);
    const name = haEl('span', {textContent: label, className: 'ha-popup-name'});
    const row = haEl('div', {className: 'ha-row ha-popup'}, name, nightLabel, brightLabel, flashLabel);
    popups.append(row);
    return {row, name, value: () => (night.checked ? 1 : 0) | (bright.checked ? 2 : 0) | (flash.checked ? 4 : 0)};
  };
  const alarmPopup = addPopup('Alarm triggered', state.alarmFlags), bellPopup = addPopup('Doorbell', state.doorbellFlags), washerPopup = addPopup('Washer finished', state.washer?.flags || 0);
  rows.icons.forEach((row, i) => {
    row.popup = addPopup('', state.icons[i].flags);
    const refresh = () => {
      const type = Number(row.type.value), open = type <= 2, done = type === 3 || type === 4 || type === 7;
      row.popup.row.hidden = !open && !done;
      row.popup.name.textContent = HA_ICONS[type] + (open ? ' left open' : ' finished');
    };
    row.type.addEventListener('change', refresh); refresh();
  });
  body.append(popups);
  body.insertBefore(energy, tiles);  // the Energy page is the main one: its entities come first

  const status = document.querySelector('#ha-status'), result = document.querySelector('#ha-result'), button = document.querySelector('#ha-save');
  const showStatus = async () => { try { const s = await (await fetch('/api/ha', {cache: 'no-store'})).json(); status.textContent = 'Connection: ' + s.status + '.' + ((mediaBox.checked || backdropBox.checked) && s.art ? ' Album art: ' + s.art + '.' : ''); } catch (_) {} haTimer = setTimeout(showStatus, 4000); };
  showStatus();
  let lastSent = '', haSaving = false, haSaveTimer;
  const build = () => {
    const body = new URLSearchParams({host: host.value, port: port.value || '8123', tls: tlsBox.checked ? '1' : '0', pages: String((homeBox.checked ? 1 : 0) | (energyBox.checked ? 2 : 0) | (mediaBox.checked ? 4 : 0) | (devicesBox.checked ? 8 : 0) | (backdropBox.checked ? 16 : 0)), pageSecs: secs.value,
      gridImportPositive: gridBox.checked ? '1' : '0', batteryDischargePositive: batteryBox.checked ? '1' : '0', doorbell: doorbell.value, washerEntity: washer.value, washerFlags: String(washerPopup.value()),
      alarmFlags: String(alarmPopup.value()), doorbellFlags: String(bellPopup.value())});
    if (token.value.trim()) body.set('token', token.value.trim());
    for (let i = 0; i < 4; i++) body.set('media' + i, rooms.value.split(',').map(id => id.trim()).filter(Boolean)[i] || '');
    rows.tiles.forEach((row, i) => { body.set('tile' + i + 'Type', row.type.value); body.set('tile' + i + 'Auto', (row.type.value === '7' ? row.ev.every(e => !e.field.value) : row.autoBox.checked) ? '1' : '0'); body.set('tile' + i + 'Entity', row.type.value === '7' ? haEvEntity(row.ev) : row.entity.value); body.set('tile' + i + 'Unit', row.unit.value); });
    rows.icons.forEach((row, i) => { body.set('icon' + i + 'Type', row.type.value); body.set('icon' + i + 'Auto', row.autoBox.checked ? '1' : '0'); body.set('icon' + i + 'Entity', row.entity.value); body.set('icon' + i + 'Alert', row.alert.value || '0'); body.set('icon' + i + 'Flags', String(row.popup.value())); });
    energyInputs.forEach((input, i) => body.set('energy' + i, input.value));
    return body;
  };
  const saveHa = async () => {
    if (haSaving) { haSaveTimer = setTimeout(saveHa, 500); return; }
    readAddress();
    const body = build(), sent = body.toString();
    if (sent === lastSent) return;
    haSaving = true; result.textContent = 'Saving…';
    try {
      const response = await fetch('/api/ha', {method: 'POST', body}); const answer = await response.json();
      result.textContent = answer.message || (response.ok ? 'Saved.' : 'Not saved.');
      result.classList.toggle('bad', !response.ok);
      if (!response.ok) message(result.textContent, true); else if (!notice.hidden && notice.classList.contains('error')) notice.hidden = true;
      if (response.ok) { lastSent = sent; if (token.value) { token.value = ''; token.placeholder = 'Saved. Type to replace it.'; lastSent = build().toString(); } clearTimeout(haTimer); setTimeout(showStatus, 1500); }
    } catch (_) { result.textContent = 'The clock did not answer.'; result.classList.add('bad'); message(result.textContent, true); }
    haSaving = false;
  };
  // Saves by itself a moment after the last change (a fix to an invalid entry is saved again the same way)
  lastSent = (readAddress(), build().toString());
  for (const type of ['input', 'change']) card.addEventListener(type, () => { clearTimeout(haSaveTimer); haSaveTimer = setTimeout(saveHa, 1500); });
  button.hidden = true;
}

// Home Assistant over MQTT (optional, off by default)
{
  const $ = (id) => document.querySelector('#mqtt-' + id);
  let timer;
  const load = async (fill) => {
    try {
      const state = await (await fetch('/api/mqtt')).json();
      $('status').textContent = state.status;
      if (fill) {
        $('enabled').checked = state.enabled; $('screen-camera').checked = state.screenCamera; $('host').value = state.host; $('port').value = state.port === 1883 ? '' : state.port;
        $('user').value = state.user; $('password').placeholder = state.passwordSet ? 'Saved: leave empty to keep' : 'No password';
      }
      clearTimeout(timer);
      if (state.enabled && !state.connected) timer = setTimeout(() => load(false), 4000);
    } catch (_) {}
  };
  $('save').addEventListener('click', async () => {
    const body = new URLSearchParams({enabled: $('enabled').checked ? '1' : '0', screenCamera: $('screen-camera').checked ? '1' : '0', host: $('host').value.trim(), port: $('port').value.trim(),
      user: $('user').value.trim(), password: $('password').value, passwordClear: $('password-clear').checked ? '1' : '0'});
    try {
      const response = await fetch('/api/mqtt', {method:'POST', body}); const result = await response.json();
      $('status').textContent = result.message;
      if (response.ok) { $('password').value = ''; $('password-clear').checked = false; setTimeout(() => load(true), 1500); }
    } catch (_) { $('status').textContent = 'Could not reach the clock.'; }
  });
  load(true);
}

async function loadStorage() {
  const summary = document.querySelector('#storage-summary'), list = document.querySelector('#storage-list'), clear = document.querySelector('#storage-clear');
  try {
    const state = await (await fetch('/api/storage', { cache: 'no-store' })).json(), kb = bytes => Math.ceil(bytes / 1024) + ' KB';
    if (state.available === false) { summary.textContent = 'The flash disk cannot be read. Formatting it below fixes that, but removes saved drawings.'; list.replaceChildren(); clear.hidden = true; return; }
    summary.textContent = kb(state.free) + ' free of ' + kb(state.total) + '. Saved drawings: ' + state.drawings.files + ' (' + kb(state.drawings.bytes) + '). Cached data: ' + state.other.files + ' file' + (state.other.files === 1 ? '' : 's') + ' (' + kb(state.other.bytes) + ').';
    list.replaceChildren(...state.other.list.map(file => { const line = document.createElement('small'); line.style.display = 'block'; line.textContent = file.n + ' · ' + (file.b / 1024).toFixed(1) + ' KB'; return line; }));
    clear.hidden = !state.other.files; clear.dataset.count = state.other.files; clear.dataset.bytes = state.other.bytes;
  } catch (_) { summary.textContent = 'Could not read the storage.'; }
}
document.querySelector('#storage-card').addEventListener('toggle', event => { if (event.target.open) loadStorage(); });
document.querySelector('#storage-clear').addEventListener('click', async event => {
  const button = event.currentTarget;
  if (!confirm('Remove ' + button.dataset.count + ' cached item(s), ' + Math.ceil(button.dataset.bytes / 1024) + ' KB? Saved drawings stay.')) return;
  try { const response = await fetch('/api/storage/clear', { method: 'POST' }); if (!response.ok) throw new Error('Could not clear the cached data.'); message('Cleared'); await loadStorage(); }
  catch (error) { message(error.message, true); }
});
function showPin(set) {
  document.querySelector('#pin-status').textContent = set ? 'A PIN is set.' : 'No PIN is set. Anyone on your network can change this clock.';
  document.querySelector('#pin-save').textContent = set ? 'Change PIN' : 'Set PIN';
  document.querySelector('#pin-remove').hidden = !set;
}
async function savePin(pin) {
  
  try {
    const response = await fetch('/api/pin', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'pin=' + encodeURIComponent(pin) });
    const data = await response.json().catch(() => ({}));
    if (!response.ok) throw new Error(data.message || 'Could not save the PIN.');
    window.umToken(data.token || '');
    document.querySelector('#pin-input').value = '';
    showPin(data.pinSet);
    message(data.message);
  } catch (error) { message(error.message, true); }
}
document.querySelector('#pin-save').addEventListener('click', () => {
  const pin = document.querySelector('#pin-input').value;
  if (!/^[0-9]{4,8}$/.test(pin)) { message('The PIN is 4 to 8 digits.', true); return; }
  savePin(pin);
});
document.querySelector('#pin-remove').addEventListener('click', () => { if (confirm('Remove the PIN? Anyone on your network can then change this clock.')) savePin(''); });
document.querySelector('#storage-reformat').addEventListener('click', async () => {
  if (!confirm('Format the flash disk? Every saved drawing and cached file is deleted and the clock restarts. This cannot be undone.')) return;
  try { const response = await fetch('/api/storage/reformat', { method: 'POST' }); if (!response.ok) throw new Error('Could not format the flash disk.'); message('Formatted. The clock restarts.'); }
  catch (error) { message(error.message, true); }
});

// Pac-Man: play yourself. Arrow keys, the full-screen touch controller or a game controller; the clock sends the direction on to the face.
(function () {
  const pad = document.querySelector('#pad'), play = document.querySelector('#play');
  if (!pad) return;
  let busy = false, queued = null;
  // Pong and Invaders take their moves over one open WebSocket (/play); until it is up, or when it cannot be opened
  // (Pac-Man has none), every move is a POST as before. Paddle positions (200..300) are sent at most every 16 ms, newest only.
  let ws = null, wsUp = false, wsDead = false, wsRetry = 0, posPending = null, posTimer = 0, posAt = 0;
  function socketOpen() {
    if (ws || wsDead || !window.WebSocket) return;
    let opened = false;
    try { ws = new WebSocket('ws://' + location.host + '/play'); } catch (_) { wsDead = true; return; }
    ws.onopen = () => {
      opened = true;
      try { const t = sessionStorage.getItem('umToken'); if (t) ws.send('t' + t); } catch (_) { /* no token: only matters with a PIN */ }
      wsUp = true; wsRetry = 0;
    };
    ws.onclose = ws.onerror = () => {
      const was = ws; ws = null; wsUp = false;
      if (was && was.readyState < 2) { try { was.close(); } catch (_) { /* gone */ } }
      if (!opened) { if (++wsRetry >= 3) wsDead = true; }   // never opened: the face has no socket, or the clock is busy; stop after a few tries
      if (!wsDead) setTimeout(socketOpen, 1000);
    };
  }
  function socketSend(dir) {
    if (dir >= 200) {
      posPending = dir;
      const wait = posAt + 16 - performance.now();
      if (wait <= 0) flushPos(); else if (!posTimer) posTimer = setTimeout(flushPos, wait);
      return;
    }
    flushPos();   // a pending position goes first, so the order is kept
    ws.send(String(dir));
  }
  function flushPos() {
    posTimer = 0;
    if (posPending === null || !wsUp) return;
    ws.send(String(posPending)); posPending = null; posAt = performance.now();
  }
  async function steer(dir) {
    if (!wsDead && !ws) socketOpen();
    if (wsUp) { try { socketSend(+dir); return; } catch (_) { /* fall back to a POST */ } }
    if (busy) { queued = dir; return; }
    busy = true;
    try { await fetch('/api/play/steer', { method: 'POST', body: new URLSearchParams({ dir }) }); } catch (_) { /* the next move tries again */ }
    busy = false;
    if (queued !== null) { const next = queued; queued = null; steer(next); }
  }
  // Android vibrates; iPhones tick when a hidden switch checkbox is toggled from a touch (iOS 17.4 and later, best effort)
  const tick = document.createElement('label');
  tick.style.cssText = 'position:absolute;width:1px;height:1px;opacity:0;overflow:hidden;pointer-events:none';
  tick.innerHTML = '<input type="checkbox" switch tabindex="-1" aria-hidden="true">';
  pad.appendChild(tick);
  const buzz = (ms) => { try { if (navigator.vibrate) navigator.vibrate(ms); else tick.click(); } catch (_) { /* no haptics on this device */ } };
  pad.addEventListener('click', (event) => { const b = event.target.closest('[data-dir]'); if (b) { buzz(25); steer(b.dataset.dir); } });
  // Full-screen controller: the joystick appears wherever a finger lands and trails the drag; letting go keeps him going.
  const ctrl = document.querySelector('#ctrl'), base = document.querySelector('#ctrl-base'), cknob = document.querySelector('#ctrl-knob');
  if (ctrl) {
    let id = null, ox = 0, oy = 0, cdir = null;
    const sens = document.querySelector('#ctrl-sens');   // 1 to 5, kept in this browser
    try { sens.value = localStorage.getItem('ctrlSens') || '3'; } catch (_) { /* the default stays */ }
    sens.addEventListener('input', () => { try { localStorage.setItem('ctrlSens', sens.value); } catch (_) { /* not kept */ } });
    const gain = () => [1, 1.5, 2.2, 3.2, 4.5][(+sens.value || 3) - 1];
    const open = (on) => { ctrl.hidden = !on; document.body.style.overflow = on ? 'hidden' : ''; if (!on) { id = null; base.hidden = true; } };
    document.querySelector('#ctrl-open').addEventListener('click', () => open(true));
    document.querySelector('#ctrl-close').addEventListener('click', () => open(false));
    document.querySelector('#ctrl-ai').addEventListener('click', () => { buzz(25); steer(-1); });
    document.addEventListener('keydown', (event) => { if (event.key === 'Escape' && !ctrl.hidden) open(false); });
    // Pong: a dial like the original paddle controller. Turning it (going round it with a finger, anywhere on the screen) moves the paddle; the angle is kept between grips.
    const dial = document.createElement('div'), mark = document.createElement('div');
    dial.style.cssText = 'position:absolute;left:50%;top:44%;width:240px;height:240px;margin:-120px 0 0 -120px;border-radius:50%;border:3px solid rgba(255,255,255,.55);background:radial-gradient(circle,rgba(255,255,255,.14),rgba(255,255,255,.04));pointer-events:none;display:none';
    mark.style.cssText = 'position:absolute;left:50%;top:10px;width:10px;height:44px;margin-left:-5px;border-radius:5px;background:var(--accent,#e8b400)';
    dial.append(mark); ctrl.append(dial);
    let dialAngle = 0, lastY = 0, lastX = 0;   // degrees, -120 to 120
    // Invaders: the same, sideways. A swipe left or right moves a slider (the cannon); any touch fires
    const track = document.createElement('div'), puck = document.createElement('div');
    track.style.cssText = 'position:absolute;left:8%;right:8%;top:44%;height:8px;margin-top:-4px;border-radius:4px;background:rgba(255,255,255,.3);pointer-events:none;display:none';
    puck.style.cssText = 'position:absolute;top:50%;width:34px;height:34px;margin:-17px 0 0 -17px;border-radius:50%;background:var(--accent,#e8b400);box-shadow:0 2px 8px rgba(0,0,0,.5)';
    track.append(puck); ctrl.append(track);
    const hint = ctrl.querySelector('p');
    const showDial = () => {
      hint.textContent = invaders ? 'Swipe left and right to move. Tap to shoot.' : pongSlide ? 'Swipe up and down to turn the dial' : 'Touch anywhere and drag';
      dial.style.display = pongSlide ? 'block' : 'none'; dial.style.transform = 'rotate(' + dialAngle + 'deg)';
      track.style.display = invaders ? 'block' : 'none'; puck.style.left = Math.round((dialAngle + 120) / 240 * 100) + '%';
    };
    new MutationObserver(showDial).observe(ctrl, { attributes: true, attributeFilter: ['hidden'] });
    const turn = (event) => {   // a swipe up or down turns the dial, wherever it is on the screen
      const d = (event.clientY - lastY) * 240 / (innerHeight * 0.5); lastY = event.clientY;
      dialAngle = Math.max(-120, Math.min(120, dialAngle + d * gain()));   // stops exactly at the ends, so reversing reacts at once
      dial.style.transform = 'rotate(' + dialAngle + 'deg)';
      const pct = Math.round((dialAngle + 120) / 240 * 100);
      if (pct !== cdir) { cdir = pct; if (pct % 4 === 0) buzz(6); steer(200 + pct); }
    };
    const slide = (event) => {   // a swipe left or right moves the slider, wherever it is on the screen
      const d = (event.clientX - lastX) * 240 / (innerWidth * 0.6); lastX = event.clientX;
      dialAngle = Math.max(-120, Math.min(120, dialAngle + d * gain()));
      puck.style.left = Math.round((dialAngle + 120) / 240 * 100) + '%';
      const pct = Math.round((dialAngle + 120) / 240 * 100);
      if (pct !== cdir) { cdir = pct; if (pct % 4 === 0) buzz(6); steer(200 + pct); }
    };
    const move = (event) => {   // the stick
      if (pongSlide) return turn(event);
      if (invaders) return slide(event);
      let dx = event.clientX - ox, dy = event.clientY - oy;
      const length = Math.hypot(dx, dy), reach = 40 / gain();
      if (length > reach) { ox += dx * (length - reach) / length; oy += dy * (length - reach) / length; dx = event.clientX - ox; dy = event.clientY - oy; base.style.left = ox + 'px'; base.style.top = oy + 'px'; }
      cknob.style.transform = 'translate(' + dx + 'px,' + dy + 'px)';
      if (Math.hypot(dx, dy) < 22 / gain()) return;
      const dir = Math.abs(dx) > Math.abs(dy) ? (dx > 0 ? 1 : 3) : (dy > 0 ? 2 : 0);
      if (dir !== cdir) { cdir = dir; buzz(12); steer(dir); }
    };
    ctrl.addEventListener('pointerdown', (event) => {
      if (event.target.closest('button')) return;
      if (invaders) { event.preventDefault(); buzz(15); steer(4); }   // every touch fires, also a second finger while the first one steers
      if (id !== null) return;
      event.preventDefault(); id = event.pointerId; ctrl.setPointerCapture(id);
      ox = event.clientX; oy = event.clientY; cdir = null; cknob.style.transform = ''; if (pongSlide) { lastY = event.clientY; showDial(); return; }
      if (invaders) { lastX = event.clientX; showDial(); return; }
      base.style.left = ox + 'px'; base.style.top = oy + 'px'; base.hidden = false; buzz(8);
    });
    ctrl.addEventListener('pointermove', (event) => { if (event.pointerId === id) move(event); });
    const end = (event) => { if (event.pointerId !== id) return; if (pongSlide || (invaders && cdir !== null)) { steer(200 + Math.round((dialAngle + 120) / 240 * 100)); /* the final place once more, in case the last move was dropped */ } id = null; base.hidden = true; };
    ctrl.addEventListener('pointerup', end); ctrl.addEventListener('pointercancel', end);
  }
  // A Bluetooth controller paired with the clock itself: status, Pair (a minute of listening) and Forget.
  const label = { off: 'The Bluetooth controller is off. Turning it on restarts the clock. It only uses memory (about 90 KB) while a controller is paired, and then everything except Home Assistant MQTT is switched off.', starting: 'Bluetooth is starting…', idle: 'Bluetooth is on but sleeping: it costs no memory until a controller is paired. Press Pair to start it (the clock restarts).', ready: 'No controller paired.', pairing: 'Switch your controller to pairing mode now…', searching: 'Paired. Waiting for the controller to be switched on.', connecting: 'Connecting…', connected: 'Controller connected.' };
  const status = document.querySelector('#pad-status'), pair = document.querySelector('#pad-pair'), forget = document.querySelector('#pad-forget'), power = document.querySelector('#pad-power');
  let btOn = false;
  let polling = 0;
  async function padState() {
    try {
      const r = await (await fetch('/api/gamepad')).json();
      status.textContent = (label[r.state] || '') + (r.state === 'connected' && r.name ? ' (' + r.name + ')' : '');
      btOn = r.enabled; power.textContent = btOn ? 'Turn Bluetooth off' : 'Turn Bluetooth on';
      pair.hidden = !btOn; forget.hidden = !r.bonded; pair.disabled = r.state === 'pairing' || r.state === 'starting';
    } catch (_) { status.textContent = ''; }
  }
  function watchPad() { if (!polling) { padState(); polling = setInterval(() => { if (play.hidden || document.hidden) return; padState(); }, 1500); } }
  if (status) {
    new MutationObserver(watchPad).observe(play, { attributes: true, attributeFilter: ['hidden'] });
    if (!play.hidden) watchPad();
    pair.addEventListener('click', async () => { try { await fetch('/api/gamepad/pair', { method: 'POST' }); } catch (_) { /* shown by the next status */ } padState(); });
    power.addEventListener('click', async () => {
      if (!confirm(btOn ? 'Turn Bluetooth off? The clock restarts.' : 'Turn Bluetooth on? The clock restarts. Once a controller is paired it uses about 90 KB of memory, and then WLED, Canvas, Animations, DLNA, notifications and telnet are switched off (Home Assistant MQTT stays). Updates restart the clock first.')) return;
      try { const r = await fetch('/api/gamepad/enable', { method: 'POST', body: new URLSearchParams({ on: btOn ? 0 : 1 }) }); if (!r.ok) throw new Error(await r.text()); message('Restarting…'); } catch (error) { message(error.message, true); }
    });
    forget.addEventListener('click', async () => { if (!confirm('Forget the paired controller?')) return; try { await fetch('/api/gamepad/forget', { method: 'POST' }); } catch (_) { /* shown by the next status */ } padState(); });
  }
  const keys = { ArrowUp: 0, ArrowRight: 1, ArrowDown: 2, ArrowLeft: 3, w: 0, d: 1, s: 2, a: 3, W: 0, D: 1, S: 2, A: 3 };
  document.addEventListener('keydown', (event) => {
    if (invaders && event.key === ' ' && !play.hidden && !/^(INPUT|SELECT|TEXTAREA|BUTTON)$/.test(document.activeElement.tagName)) { event.preventDefault(); steer(4); return; }
    if (play.hidden || event.repeat || event.ctrlKey || event.metaKey || !(event.key in keys) || /^(INPUT|SELECT|TEXTAREA)$/.test(document.activeElement.tagName)) return;
    event.preventDefault(); steer(keys[event.key]);
  });
  // A game controller paired with this phone or computer (Bluetooth or USB): d-pad or left stick steers, Start gives him back to the clock.
  let last = null;
  setInterval(() => {
    if (play.hidden || document.hidden) return;
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    for (const g of pads) {
      if (!g) continue;
      const on = (i) => g.buttons[i] && g.buttons[i].pressed, x = g.axes[0] || 0, y = g.axes[1] || 0;
      let dir = null;
      if (on(12) || y < -0.5) dir = 0; else if (on(15) || x > 0.5) dir = 1; else if (on(13) || y > 0.5) dir = 2; else if (on(14) || x < -0.5) dir = 3;
      else if (on(9)) dir = -1;
      if (dir !== null && dir !== last) steer(dir);
      last = dir;
      return;
    }
  }, 50);
})();

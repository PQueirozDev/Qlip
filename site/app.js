// Qlip — site. Sem dependências.

// Endereços num lugar só: trocar aqui se o repositório mudar de nome.
const REPO = "PQueirozDev/Qlip";
const LINKS = {
  repo: `https://github.com/${REPO}`,
  releases: `https://github.com/${REPO}/releases`,
  // Toda release publica o zip portátil com este nome fixo, então o link "latest" sempre funciona.
  download: `https://github.com/${REPO}/releases/latest/download/Qlip-win64.zip`,
  setup: `https://github.com/${REPO}/releases/latest/download/Qlip-Setup.exe`,
};
document.querySelectorAll("[data-link]").forEach((a) => {
  const href = LINKS[a.dataset.link];
  if (href) a.href = href;
});
document.querySelectorAll("[data-repo-url]").forEach((el) => (el.textContent = LINKS.repo));

// Versão: a da última release publicada (fica a do HTML se a API não responder).
fetch(`https://api.github.com/repos/${REPO}/releases/latest`)
  .then((r) => (r.ok ? r.json() : null))
  .then((release) => {
    const tag = release?.tag_name?.replace(/^v/, "");
    if (tag) document.querySelectorAll("[data-version]").forEach((el) => (el.textContent = tag));
  })
  .catch(() => {});

const reduced = window.matchMedia("(prefers-reduced-motion: reduce)").matches;

// --- Replay: frames passando, buffer dos últimos N segundos e o F8 -----------------

const framesEl = document.querySelector("[data-frames]");
const windowEl = document.querySelector("[data-window]");
const infoEl = document.querySelector("[data-rp-info]");
const clipsEl = document.querySelector("[data-clips]");
const saveBtn = document.querySelector("[data-save]");

// Largura do buffer na faixa para cada duração (a faixa representa ~2,5 min).
const WINDOW_WIDTH = { 15: "16%", 30: "28%", 60: "48%", 120: "78%" };
let seconds = 30;
let frameCount = 0;

function addFrame() {
  const bar = document.createElement("i");
  // Um keyframe a cada 6 barras: o clipe sempre começa num deles.
  if (frameCount++ % 6 === 0) bar.className = "key";
  bar.style.height = `${55 + Math.round(Math.random() * 45)}%`;
  bar.style.alignSelf = "center";
  framesEl.appendChild(bar);
  while (framesEl.children.length > 140) framesEl.firstElementChild.remove();
}
for (let i = 0; i < 90; i++) addFrame();
if (!reduced) setInterval(addFrame, 140);

function setDuration(value) {
  seconds = value;
  windowEl.style.width = WINDOW_WIDTH[value];
  infoEl.textContent = `últimos ${value} s em memória`;
  document.querySelectorAll("[data-seconds]").forEach((b) => {
    const on = Number(b.dataset.seconds) === value;
    b.classList.toggle("on", on);
    b.setAttribute("aria-pressed", String(on));
  });
}
document.querySelectorAll("[data-seconds]").forEach((b) => b.addEventListener("click", () => setDuration(Number(b.dataset.seconds))));
setDuration(30);

function pad(n) {
  return String(n).padStart(2, "0");
}

function saveClip() {
  windowEl.classList.remove("flash");
  void windowEl.offsetWidth; // reinicia a animação
  windowEl.classList.add("flash");

  clipsEl.querySelector(".empty")?.remove();
  const now = new Date();
  const name = `Qlip_Clipe_${pad(now.getHours())}-${pad(now.getMinutes())}-${pad(now.getSeconds())}.mp4`;
  const item = document.createElement("li");
  const label = document.createElement("b");
  label.textContent = name;
  const meta = document.createElement("span");
  meta.textContent = `${seconds} s · salvo`;
  item.append(label, meta);
  clipsEl.prepend(item);
  while (clipsEl.children.length > 4) clipsEl.lastElementChild.remove();
}
saveBtn.addEventListener("click", saveClip);
document.addEventListener("keydown", (event) => {
  if (event.key === "F8") {
    event.preventDefault();
    saveClip();
  }
});

// --- Copiar comandos --------------------------------------------------------------

const copyBtn = document.querySelector("[data-copy]");
copyBtn?.addEventListener("click", async () => {
  const pre = document.querySelector("[data-copy-src]");
  const text = pre.innerText
    .split("\n")
    .filter((l) => !l.trim().startsWith("#"))
    .map((l) => l.replace(/\s+#.*$/, ""))
    .join("\n");
  try {
    await navigator.clipboard.writeText(text);
    copyBtn.textContent = "copiado";
  } catch {
    const range = document.createRange();
    range.selectNodeContents(pre);
    const sel = getSelection();
    sel.removeAllRanges();
    sel.addRange(range);
    copyBtn.textContent = "selecionado";
  }
  setTimeout(() => (copyBtn.textContent = "copiar"), 1800);
});

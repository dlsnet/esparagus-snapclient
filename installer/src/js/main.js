import "../scss/main.scss";
import { initBootstrap } from "./bootstrap.js";

initBootstrap({
  tooltip: true,
  popover: true,
  toasts: true,
});

function selectCard(card) {
  const installButton = document.getElementById("button_web_install");
  const cards = Array.from(document.querySelectorAll(".firmware-card"));

  cards.forEach((elem) => {
    const selected = elem === card;
    elem.classList.toggle("border-primary", selected);
    elem.classList.toggle("border-secondary", !selected);

    const header = elem.querySelector(".card-header");
    header.classList.toggle("bg-primary", selected);
    header.classList.toggle("text-white", selected);
    header.classList.toggle("bg-light", !selected);

    const radio = elem.querySelector('input[type="radio"]');
    if (radio) {
      radio.checked = selected;
    }
  });

  installButton.setAttribute("manifest", card.dataset.manifest);
}

function initInstallerSelection() {
  const cards = Array.from(document.querySelectorAll(".firmware-card"));
  if (cards.length === 0) {
    return;
  }

  cards.forEach((card) => {
    card.addEventListener("click", () => selectCard(card));
  });

  const initiallySelected =
    document.querySelector('.firmware-card[data-default="true"]') || cards[0];
  selectCard(initiallySelected);
}

document.addEventListener("DOMContentLoaded", initInstallerSelection);

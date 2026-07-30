(() => {
  "use strict";

  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => Array.from(root.querySelectorAll(selector));
  const siteUrl = (path) => new URL(path, document.baseURI).toString();

  function getPath(obj, path) {
    return path.split(".").reduce((value, key) => (value == null ? undefined : value[key]), obj);
  }

  function bindText(content) {
    $$('[data-bind]').forEach((el) => {
      const value = getPath(content, el.getAttribute('data-bind'));
      if (typeof value === "string") el.textContent = value;
    });
  }

  function renderBenefits(benefits) {
    const list = $("#course-benefits");
    list.innerHTML = "";
    (benefits || []).forEach((benefit) => {
      const item = document.createElement("li");
      const title = document.createElement("strong");
      title.textContent = benefit.title;
      const detail = document.createElement("span");
      detail.textContent = benefit.detail;
      item.append(title, detail);
      list.append(item);
    });
  }

  function renderCards(steps, guideSteps) {
    const list = $("#course-install-steps");
    list.innerHTML = "";
    (steps || []).forEach((step, index) => {
      const item = document.createElement("li");
      const title = document.createElement("h3");
      title.textContent = step.title;
      const detail = document.createElement("p");
      detail.textContent = step.detail;
      const guide = guideSteps?.[index];
      if (guide) {
        const help = document.createElement("button");
        help.type = "button";
        help.className = "course-step-help";
        help.textContent = "!";
        help.setAttribute("aria-label", `查看“${step.title}”图文说明`);
        help.title = "查看图片说明";
        help.addEventListener("click", () => openGuideDialog(guide, index));
        item.append(title, detail, help);
      } else {
        item.append(title, detail);
      }
      list.append(item);
    });
  }

  function openGuideDialog(step, index) {
    const dialog = $("#course-guide-dialog");
    $("#course-guide-dialog-step").textContent = `第 ${index + 1} 步`;
    $("#course-guide-dialog-title").textContent = step.title;
    $("#course-guide-dialog-detail").textContent = step.detail;
    const image = $("#course-guide-dialog-image");
    image.src = siteUrl(step.image);
    image.alt = step.imageAlt || step.title;
    const hotspots = $("#course-guide-hotspots");
    hotspots.innerHTML = "";
    (step.hotspots || []).forEach((spot, spotIndex) => {
      const marker = document.createElement("span");
      const direction = ["up", "right", "down", "left"].includes(spot.direction)
        ? spot.direction
        : "right";
      marker.className = `course-guide-callout is-${direction}`;
      marker.textContent = spot.label || String(spotIndex + 1);
      marker.style.setProperty("--left", spot.left);
      marker.style.setProperty("--top", spot.top);
      hotspots.append(marker);
    });
    if (!dialog.open) dialog.showModal();
  }

  function setupGuideDialog() {
    const dialog = $("#course-guide-dialog");
    $("#course-guide-dialog-close").addEventListener("click", () => dialog.close());
    dialog.addEventListener("click", (event) => {
      if (event.target === dialog) dialog.close();
    });
  }

  function formatBytes(bytes) {
    if (!bytes || Number.isNaN(bytes)) return "";
    return `${(bytes / 1024 / 1024).toFixed(1)} MB`;
  }

  async function fetchRelease() {
    try {
      const response = await fetch(siteUrl("api/releases/latest"), { headers: { Accept: "application/json" } });
      if (!response.ok) throw new Error("无法获取版本信息");
      return await response.json();
    } catch {
      return null;
    }
  }

  function setupDownload(content, release) {
    const extension = content.downloads.browserExtension;
    const link = $("#course-download");
    const meta = $("#course-download-meta");
    const asset = release?.assets?.[extension.asset];
    if (release?.tag && asset) {
      link.href = siteUrl(`download/${encodeURIComponent(release.tag)}/${encodeURIComponent(extension.asset)}`);
      meta.textContent = `${release.tag} · ${formatBytes(asset.size)}`;
      return;
    }
    link.href = `${content.site.latestReleaseUrl}/download/${encodeURIComponent(extension.asset)}`;
    link.target = "_blank";
    link.rel = "noopener";
    meta.textContent = "下载后按下方步骤添加到浏览器";
  }

  function setupMobileNav() {
    const toggle = $(".nav-toggle");
    const nav = $("#mobile-nav");
    toggle.addEventListener("click", () => {
      const expanded = toggle.getAttribute("aria-expanded") === "true";
      toggle.setAttribute("aria-expanded", String(!expanded));
      nav.hidden = expanded;
    });
    $$("#mobile-nav a").forEach((link) => link.addEventListener("click", () => {
      toggle.setAttribute("aria-expanded", "false");
      nav.hidden = true;
    }));
    $("#mobile-support-open").addEventListener("click", () => {
      toggle.setAttribute("aria-expanded", "false");
      nav.hidden = true;
      openSupportDialog();
    });
  }

  function openSupportDialog() {
    const dialog = $("#support-dialog");
    if (!dialog.open) dialog.showModal();
  }

  function setupSupportDialog() {
    const dialog = $("#support-dialog");
    ["#support-open", "#top-support-open"].forEach((selector) => {
      $(selector).addEventListener("click", openSupportDialog);
    });
    $("#support-close").addEventListener("click", () => dialog.close());
    dialog.addEventListener("click", (event) => {
      if (event.target === dialog) dialog.close();
    });

    const methods = [
      { id: "wechat", label: "微信扫码赞赏", src: "assets/support/wechat-payment.jpg", alt: "微信收款码" },
      { id: "alipay", label: "支付宝扫码赞赏", src: "assets/support/alipay-payment.jpg", alt: "支付宝收款码" },
    ];
    let activeIndex = 0;
    const code = $("#payment-code");
    const caption = $("#payment-caption");
    const tabs = $$(".payment-tab", dialog);
    const selectPayment = (index) => {
      activeIndex = (index + methods.length) % methods.length;
      const method = methods[activeIndex];
      code.src = method.src;
      code.alt = method.alt;
      caption.textContent = method.label;
      tabs.forEach((tab) => {
        const selected = tab.dataset.payment === method.id;
        tab.classList.toggle("is-active", selected);
        tab.setAttribute("aria-pressed", String(selected));
      });
    };
    tabs.forEach((tab, index) => tab.addEventListener("click", () => selectPayment(index)));
    $("#payment-previous").addEventListener("click", () => selectPayment(activeIndex - 1));
    $("#payment-next").addEventListener("click", () => selectPayment(activeIndex + 1));
  }

  async function init() {
    const response = await fetch(siteUrl("content.json"));
    const content = await response.json();
    const course = content.courseCompanion;
    const extension = content.downloads.browserExtension;

    bindText(content);
    renderBenefits(course.benefits);
    renderCards(course.steps, extension.guideSteps);
    $("#course-compatibility").textContent = course.compatibility || "";
    $("#course-purpose").textContent = extension.purpose || "";
    setupMobileNav();
    setupSupportDialog();
    setupGuideDialog();
    setupDownload(content, await fetchRelease());
  }

  document.addEventListener("DOMContentLoaded", init);
})();

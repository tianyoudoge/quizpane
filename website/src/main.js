(() => {
  "use strict";

  const state = {
    content: null,
    release: null,
    carouselIndex: 0,
    carouselTimer: null,
  };

  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => Array.from(root.querySelectorAll(selector));
  // 官网既可以部署在域名根目录，也可以挂载在 /quizpane/ 之类的子路径下。
  // document.baseURI 会保留当前目录，避免下载 API 跳回个人主页根路由。
  const siteUrl = (path) => new URL(path, document.baseURI).toString();

  function getPath(obj, path) {
    return path.split(".").reduce((value, key) => (value == null ? undefined : value[key]), obj);
  }

  function bindText(content) {
    $$("[data-bind]").forEach((el) => {
      const value = getPath(content, el.getAttribute("data-bind"));
      if (typeof value === "string") {
        el.textContent = value;
      }
    });
  }

  function renderTrust(trust) {
    const list = $("#trust-list");
    list.innerHTML = "";
    trust.forEach((item) => {
      const li = document.createElement("li");
      const tag = document.createElement("span");
      tag.className = "trust-tag";
      tag.textContent = item.tag || "";
      const h3 = document.createElement("h3");
      h3.textContent = item.title;
      const p = document.createElement("p");
      p.textContent = item.detail;
      li.append(tag, h3, p);
      list.append(li);
    });
  }

  function renderCarousel(slides) {
    const track = $("#carousel-track");
    const dots = $("#carousel-dots");
    track.innerHTML = "";
    dots.innerHTML = "";

    slides.forEach((slide, index) => {
      const item = document.createElement("div");
      item.className = "carousel-slide";
      item.setAttribute("role", "group");
      item.setAttribute("aria-roledescription", "slide");
      item.setAttribute("aria-label", `${index + 1} / ${slides.length}: ${slide.title}`);
      item.id = `carousel-slide-${slide.id}`;

      const img = document.createElement("img");
      img.src = slide.shot;
      img.alt = slide.title;
      img.loading = "lazy";
      img.width = 960;
      img.height = 600;

      const copy = document.createElement("div");
      copy.className = "carousel-slide-copy";
      const h3 = document.createElement("h3");
      h3.textContent = slide.title;
      const p = document.createElement("p");
      p.textContent = slide.detail;
      copy.append(h3, p);

      item.append(img, copy);
      track.append(item);

      const dot = document.createElement("button");
      dot.type = "button";
      dot.className = "carousel-dot";
      dot.setAttribute("role", "tab");
      dot.setAttribute("aria-label", `跳转到第 ${index + 1} 张`);
      dot.setAttribute("aria-selected", index === 0 ? "true" : "false");
      dot.addEventListener("click", () => goToSlide(index, true));
      dots.append(dot);
    });
  }

  function goToSlide(index, userInitiated) {
    const track = $("#carousel-track");
    const slides = $$(".carousel-slide", track);
    if (!slides.length) return;
    const next = (index + slides.length) % slides.length;
    state.carouselIndex = next;
    track.scrollTo({ left: slides[next].offsetLeft, behavior: userInitiated ? "smooth" : "auto" });
    $$(".carousel-dot").forEach((dot, i) => dot.setAttribute("aria-selected", i === next ? "true" : "false"));
  }

  function prefersReducedMotion() {
    return window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  }

  function startAutoplay() {
    stopAutoplay();
    if (prefersReducedMotion()) return;
    state.carouselTimer = window.setInterval(() => {
      goToSlide(state.carouselIndex + 1, true);
    }, 6000);
  }

  function stopAutoplay() {
    if (state.carouselTimer) {
      window.clearInterval(state.carouselTimer);
      state.carouselTimer = null;
    }
  }

  function setupCarousel() {
    const carousel = $("#carousel");
    $("#carousel-prev").addEventListener("click", () => goToSlide(state.carouselIndex - 1, true));
    $("#carousel-next").addEventListener("click", () => goToSlide(state.carouselIndex + 1, true));

    ["mouseenter", "focusin"].forEach((evt) => carousel.addEventListener(evt, stopAutoplay));
    ["mouseleave", "focusout"].forEach((evt) => carousel.addEventListener(evt, startAutoplay));

    const track = $("#carousel-track");
    track.addEventListener("keydown", (event) => {
      if (event.key === "ArrowRight") goToSlide(state.carouselIndex + 1, true);
      if (event.key === "ArrowLeft") goToSlide(state.carouselIndex - 1, true);
    });

    startAutoplay();
  }

  function renderWorkflow(workflow) {
    const list = $("#workflow-steps");
    list.innerHTML = "";
    workflow.steps.forEach((step) => {
      const li = document.createElement("li");
      const h3 = document.createElement("h3");
      h3.textContent = step.title;
      const p = document.createElement("p");
      p.textContent = step.detail;
      li.append(h3, p);
      list.append(li);
    });
  }

  function formatBytes(bytes) {
    if (!bytes || Number.isNaN(bytes)) return "";
    const units = ["B", "KB", "MB", "GB"];
    let value = bytes;
    let unitIndex = 0;
    while (value >= 1024 && unitIndex < units.length - 1) {
      value /= 1024;
      unitIndex += 1;
    }
    return `${value.toFixed(unitIndex === 0 ? 0 : 1)} ${units[unitIndex]}`;
  }

  function formatDate(iso) {
    if (!iso) return "";
    try {
      const d = new Date(iso);
      return d.toLocaleDateString("zh-CN", { year: "numeric", month: "2-digit", day: "2-digit" });
    } catch {
      return "";
    }
  }

  function renderDownloads(downloads, release) {
    const grid = $("#download-grid");
    grid.innerHTML = "";
    const tag = release && release.tag ? release.tag : null;

    downloads.platforms.forEach((platform) => {
      const card = document.createElement("div");
      card.className = "download-card";

      const icon = document.createElement("img");
      icon.className = "icon";
      icon.src = `assets/icons/${platform.icon}.svg`;
      icon.alt = "";
      icon.width = 28;
      icon.height = 28;

      const h3 = document.createElement("h3");
      h3.textContent = platform.label;
      const sub = document.createElement("p");
      sub.className = "sublabel";
      sub.textContent = platform.sublabel;

      const meta = document.createElement("p");
      meta.className = "platform-meta";
      const assetInfo = release && release.assets ? release.assets[platform.asset] : null;
      if (assetInfo) {
        const parts = [formatBytes(assetInfo.size), assetInfo.sha256 ? `SHA-256: ${assetInfo.sha256.slice(0, 12)}…` : ""].filter(Boolean);
        meta.textContent = parts.join(" · ");
      }

      const link = document.createElement(platform.choices ? "button" : "a");
      link.className = "btn btn-primary";
      link.textContent = platform.choices ? "选择机型" : "下载";
      if (platform.choices) {
        link.type = "button";
        link.addEventListener("click", () => openMacosDownloadDialog(platform, release, downloads.releaseUrlFallback));
      } else if (tag) {
        link.href = siteUrl(`download/${encodeURIComponent(tag)}/${encodeURIComponent(platform.asset)}`);
      } else {
        link.href = downloads.releaseUrlFallback || "#";
        link.target = "_blank";
        link.rel = "noopener";
      }

      card.append(icon, h3, sub, meta, link);
      grid.append(card);
    });
    const extensionCard = createBrowserExtensionCard(downloads.browserExtension, release);
    if (extensionCard) grid.append(extensionCard);
  }

  function createBrowserExtensionCard(extension, release) {
    if (!extension) return null;
    const card = document.createElement("div");
    card.className = "download-card extension-download-card";
    const icon = document.createElement("img");
    icon.className = "icon";
    icon.src = "assets/icons/quizpane-app-icon.png";
    icon.alt = "";
    icon.width = 28;
    icon.height = 28;
    const title = document.createElement("h3");
    title.textContent = extension.title;
    const guide = document.createElement("button");
    guide.type = "button";
    guide.className = "extension-guide-button";
    guide.setAttribute("aria-label", "查看安装与更新说明");
    guide.textContent = "!";
    guide.addEventListener("click", () => openExtensionInstallDialog());
    const titleRow = document.createElement("div");
    titleRow.className = "extension-title-row";
    titleRow.append(title, guide);
    const detail = document.createElement("p");
    detail.className = "sublabel";
    detail.textContent = extension.detail;
    const link = document.createElement("a");
    link.className = "btn btn-primary";
    link.textContent = "下载";
    const assetInfo = release?.assets?.[extension.asset];
    if (release?.tag && assetInfo) {
      link.href = siteUrl(`download/${encodeURIComponent(release.tag)}/${encodeURIComponent(extension.asset)}`);
    } else {
      link.href = `${state.content.site.latestReleaseUrl}/download/${encodeURIComponent(extension.asset)}`;
      link.target = "_blank";
      link.rel = "noopener";
    }
    link.addEventListener("click", () => window.setTimeout(openExtensionInstallDialog, 0));
    const meta = document.createElement("p");
    meta.className = "platform-meta";
    meta.textContent = assetInfo ? formatBytes(assetInfo.size) : "";
    card.append(icon, titleRow, detail, meta, link);
    renderExtensionInstallGuide(extension);
    return card;
  }

  function renderGuideSteps(selector, steps) {
    const list = $(selector);
    list.innerHTML = "";
    (steps || []).forEach((step) => {
      const item = document.createElement("li");
      item.textContent = step;
      list.append(item);
    });
  }

  function renderExtensionInstallGuide(extension) {
    $("#extension-purpose-title").textContent = extension.purposeTitle || "它能帮你做什么？";
    $("#extension-purpose-copy").textContent = extension.purpose || "";
    $("#extension-guide-install-title").textContent = extension.installTitle || "首次安装";
    $("#extension-guide-install-hint").textContent = extension.installHint || "";
    $("#extension-guide-use-title").textContent = extension.useTitle || "安装后怎么用？";
    $("#extension-guide-update-title").textContent = extension.updateTitle || "更新已有扩展";
    renderGuideSteps("#extension-guide-install-steps", extension.steps);
    renderGuideSteps("#extension-guide-use-steps", extension.useSteps);
    renderGuideSteps("#extension-guide-update-steps", extension.updateSteps);
  }

  function openExtensionInstallDialog() {
    const dialog = $("#extension-install-dialog");
    if (!dialog.open) dialog.showModal();
  }

  function setupExtensionInstallDialog() {
    const dialog = $("#extension-install-dialog");
    $("#extension-install-close").addEventListener("click", () => dialog.close());
    dialog.addEventListener("click", (event) => {
      if (event.target === dialog) dialog.close();
    });
  }

  function openMacosDownloadDialog(platform, release, fallbackUrl) {
    const dialog = $("#macos-download-dialog");
    const list = $("#macos-choice-list");
    const tag = release?.tag;
    list.innerHTML = "";
    platform.choices.forEach((choice) => {
      const link = document.createElement("a");
      link.className = "macos-choice";
      link.href = tag
        ? siteUrl(`download/${encodeURIComponent(tag)}/${encodeURIComponent(choice.asset)}`)
        : fallbackUrl;
      if (!tag) {
        link.target = "_blank";
        link.rel = "noopener";
      }
      const title = document.createElement("strong");
      title.textContent = choice.label;
      const detail = document.createElement("span");
      detail.textContent = choice.detail;
      link.append(title, detail);
      list.append(link);
    });
    if (!dialog.open) dialog.showModal();
  }

  function setupMacosDownloadDialog() {
    const dialog = $("#macos-download-dialog");
    $("#macos-download-close").addEventListener("click", () => dialog.close());
    dialog.addEventListener("click", (event) => {
      if (event.target === dialog) dialog.close();
    });
  }

  function renderReleaseMeta(release) {
    const el = $("#release-meta");
    if (!release) {
      el.textContent = "无法获取最新版本信息，请直接前往 GitHub Release 页面下载。";
      return;
    }
    const bits = [release.tag, formatDate(release.publishedAt)].filter(Boolean);
    el.textContent = bits.length ? `最新版本：${bits.join(" · 发布于 ")}` : "";
  }

  function renderPrivacy(privacy) {
    const list = $("#privacy-points");
    list.innerHTML = "";
    privacy.points.forEach((point) => {
      const li = document.createElement("li");
      li.textContent = point;
      list.append(li);
    });
  }

  function renderContact(contact) {
    const grid = $("#contact-grid");
    grid.innerHTML = "";
    contact.items.forEach((item) => {
      const isSupport = item.action === "support";
      const link = document.createElement(isSupport ? "button" : "a");
      link.className = `contact-card${isSupport ? " contact-support" : ""}`;
      if (isSupport) {
        link.type = "button";
        link.addEventListener("click", () => openSupportDialog());
      } else {
        link.href = item.url;
      }
      if (!isSupport && item.url.startsWith("http")) {
        link.target = "_blank";
        link.rel = "noopener";
      }
      const title = document.createElement("strong");
      title.textContent = item.label;
      const detail = document.createElement("span");
      detail.textContent = item.detail;
      link.append(title, detail);
      grid.append(link);
    });
  }

  function renderFooter(footer) {
    $("#footer-note").textContent = `${footer.version} · ${footer.note}`;
    $("#footer-copyright").textContent = footer.copyright;
    const nav = $("#footer-links");
    nav.innerHTML = "";
    footer.links.forEach((link) => {
      const a = document.createElement("a");
      a.href = link.url;
      a.textContent = link.label;
      a.target = "_blank";
      a.rel = "noopener";
      nav.append(a);
    });
  }

  function setupMobileNav() {
    const toggle = $(".nav-toggle");
    const nav = $("#mobile-nav");
    toggle.addEventListener("click", () => {
      const expanded = toggle.getAttribute("aria-expanded") === "true";
      toggle.setAttribute("aria-expanded", String(!expanded));
      nav.hidden = expanded;
    });
    $$("#mobile-nav a").forEach((a) =>
      a.addEventListener("click", () => {
        toggle.setAttribute("aria-expanded", "false");
        nav.hidden = true;
      })
    );
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
    const close = $("#support-close");
    ["#support-open", "#top-support-open"].forEach((selector) => {
      $(selector).addEventListener("click", openSupportDialog);
    });
    close.addEventListener("click", () => dialog.close());
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

  async function fetchRelease() {
    try {
      const response = await fetch(siteUrl("api/releases/latest"), {
        headers: { Accept: "application/json" },
      });
      if (!response.ok) throw new Error(`status ${response.status}`);
      return await response.json();
    } catch {
      return null;
    }
  }

  async function init() {
    const response = await fetch("content.json");
    const content = await response.json();
    state.content = content;

    document.title = content.site.title;
    bindText(content);
    renderTrust(content.trust);
    renderCarousel(content.carousel);
    renderWorkflow(content.workflow);
    renderPrivacy(content.privacy);
    renderContact(content.contact);
    renderFooter(content.footer);
    setupMobileNav();
    setupSupportDialog();
    setupExtensionInstallDialog();
    setupMacosDownloadDialog();
    setupCarousel();

    content.downloads.releaseUrlFallback = content.site.latestReleaseUrl;
    renderDownloads(content.downloads, null);
    renderReleaseMeta(null);

    const release = await fetchRelease();
    state.release = release;
    renderDownloads(content.downloads, release);
    renderReleaseMeta(release);
  }

  document.addEventListener("DOMContentLoaded", init);
})();

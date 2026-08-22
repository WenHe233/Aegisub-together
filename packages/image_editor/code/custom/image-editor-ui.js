(function () {
	"use strict";

	var hiddenSelectionCommands = new Set([
		"Remove BG", "Subject", "Select Subject",
		"Háttér eltávolítása", "Tárgy", "Tárgy kijelölése"
	]);
	var windowRemovedLabels = new Set([
		"More", "Plugins", "Továbbiak", "Bővítmények",
		"Local drive", "Local Drive", "Local storage", "Local Storage",
		"Helyi meghajtó"
	]);
	var localizedPhrases = new Map();
	var localizationLoading = false;
	var openAndPlacePending = false;
	var shortcutPending = false;
	var documentCount = 0;
	var originalMoreButton = null;
	var navigationInstalled = false;
	var scaledWheelEvents = new WeakSet();

	function labelsIn(root) {
		return root && root.querySelectorAll ? root.querySelectorAll("span.label") : [];
	}

	function topMenuButtons() {
		var group = document.querySelector(".topbar > span:first-child");
		return group ? group.querySelectorAll(":scope > button") : [];
	}

	function hideControl(control) {
		if (!control) return;
		control.style.display = "none";
		control.setAttribute("aria-hidden", "true");
		control.tabIndex = -1;
	}

	function activateEditorControl(control) {
		if (!control) return;
		var bounds = control.getBoundingClientRect();
		var clientX = bounds.left + bounds.width / 2;
		var clientY = bounds.top + bounds.height / 2;
		if (window.PointerEvent) {
			control.dispatchEvent(new PointerEvent("pointerdown", {
				bubbles: true, button: 0, buttons: 1, pointerId: 1,
				pointerType: "mouse", isPrimary: true,
				clientX: clientX, clientY: clientY
			}));
			control.dispatchEvent(new PointerEvent("pointerup", {
				bubbles: true, button: 0, buttons: 0, pointerId: 1,
				pointerType: "mouse", isPrimary: true,
				clientX: clientX, clientY: clientY
			}));
		}
		else {
			control.dispatchEvent(new MouseEvent("mousedown", {
				bubbles: true, button: 0, buttons: 1,
				clientX: clientX, clientY: clientY
			}));
			control.dispatchEvent(new MouseEvent("mouseup", {
				bubbles: true, button: 0, buttons: 0,
				clientX: clientX, clientY: clientY
			}));
		}
	}

	function activateMenuItem(control) {
		if (!control) return;
		var bounds = control.getBoundingClientRect();
		var clientX = bounds.left + bounds.width / 2;
		var clientY = bounds.top + bounds.height / 2;
		control.dispatchEvent(new MouseEvent("mousedown", {
			bubbles: true, button: 0, buttons: 1,
			clientX: clientX, clientY: clientY
		}));
		control.dispatchEvent(new MouseEvent("mouseup", {
			bubbles: true, button: 0, buttons: 0,
			clientX: clientX, clientY: clientY
		}));
		control.dispatchEvent(new MouseEvent("click", {
			bubbles: true, button: 0, buttons: 0,
			clientX: clientX, clientY: clientY
		}));
	}

	function language() {
		try {
			return (window.parent.__imageEditorLanguage || "en").toLowerCase();
		}
		catch (_) {
			return "en";
		}
	}

	function localizedText(english, hungarian) {
		var translated = localizedPhrases.get(english);
		if (translated && translated !== english) return translated;
		return language().indexOf("hu") === 0 && hungarian ? hungarian : english;
	}

	function normalizedLabel(value) {
		return String(value || "").trim().replace(/(?:\.{3}|…)\s*$/, "").trim();
	}

	function matchesLocalized(value, english, hungarian) {
		var normalized = normalizedLabel(value);
		return normalized === normalizedLabel(english) ||
			normalized === normalizedLabel(localizedText(english, hungarian));
	}

	function parseLanguageTable(source) {
		var position = 0;

		function parseArray() {
			var values = [];
			position++;
			while (position < source.length && source[position] !== "]") {
				if (source[position] === ";") {
					values.push("");
					position++;
					continue;
				}
				var value;
				if (source[position] === "[") value = parseArray();
				else {
					var start = position;
					while (position < source.length &&
						source[position] !== ";" && source[position] !== "]") position++;
					value = source.slice(start, position);
				}
				values.push(value);
				if (source[position] === ";") {
					position++;
					if (source[position] === "]") values.push("");
				}
			}
			if (source[position] === "]") position++;
			return values;
		}

		var root = [];
		while (position < source.length) {
			if (source[position] === ";") {
				root.push("");
				position++;
				continue;
			}
			var value;
			if (source[position] === "[") value = parseArray();
			else {
				var start = position;
				while (position < source.length && source[position] !== ";") position++;
				value = source.slice(start, position);
			}
			root.push(value);
			if (source[position] === ";") position++;
		}
		return root;
	}

	function createLocalizationMap(englishTable, translatedTable) {
		var result = new Map();
		function visit(english, translated) {
			if (Array.isArray(english)) {
				for (var i = 0; i < english.length; i++)
					visit(english[i], Array.isArray(translated) ? translated[i] : undefined);
				return;
			}
			var key = String(english || "").split("::", 1)[0];
			var value = typeof translated === "string" ?
				translated.split("::", 1)[0] : "";
			if (key && !result.has(key)) result.set(key, value || key);
		}
		visit(englishTable, translatedTable);
		return result;
	}

	function languageTableIndex() {
		if (!window.LNG || !Array.isArray(window.LNG.langs)) return 0;
		var requested = language();
		var base = requested.split("-", 1)[0];
		var fallback = 0;
		for (var i = 0; i < window.LNG.langs.length; i++) {
			var code = String(window.LNG.langs[i].code || "").toLowerCase();
			if (code === requested) return i;
			if (code.split("-", 1)[0] === base) fallback = i;
		}
		return fallback;
	}

	function languageTableSource(index) {
		if (window.LNG && window.LNG.tables &&
			typeof window.LNG.tables[index] === "string" &&
			window.LNG.tables[index].length > 100)
			return Promise.resolve(window.LNG.tables[index]);
		return fetch("code/lang/" + index + ".js").then(function (response) {
			if (!response.ok) throw new Error("Language table could not be loaded");
			return response.text();
		});
	}

	function loadLocalization() {
		if (localizationLoading || localizedPhrases.size) return;
		localizationLoading = true;
		var tableIndex = languageTableIndex();
		Promise.all([languageTableSource(0), languageTableSource(tableIndex)])
			.then(function (tables) {
				localizedPhrases = createLocalizationMap(
					parseLanguageTable(tables[0]), parseLanguageTable(tables[1]));
			})
			.catch(function () {})
			.finally(function () {
				localizationLoading = false;
				scanDocument();
			});
	}

	function imageImportText() {
		if (language().indexOf("hu") === 0) return "Kép importálása...";
		return localizedText("Open & Place", "Megnyitás és elhelyezés") + "...";
	}

	function hideTopBarItems() {
		var buttons = topMenuButtons();
		if (buttons.length) hideControl(buttons[0]);

		var actions = document.querySelectorAll(".topbar > span:last-child > button");
		for (var i = 0; i < actions.length; i++) {
			var title = (actions[i].getAttribute("title") ||
				actions[i].getAttribute("aria-label") ||
				actions[i].textContent || "").trim();
			var command = title.replace(/\s*\([^)]*\)\s*$/, "").trim();
			if (matchesLocalized(command, "Search", "Keresés") ||
				matchesLocalized(command, "Find", "Keresés") ||
				matchesLocalized(command, "Fullscreen", "Teljes képernyő") ||
				matchesLocalized(command, "Full screen", "Teljes képernyő"))
				hideControl(actions[i]);
		}
	}

	function installShortcutButton() {
		var installed = document.querySelector(".image-editor-shortcuts");
		if (installed) {
			installed.textContent = localizedText(
				"Keyboard Shortcuts", "Billentyűparancsok");
			return;
		}
		var buttons = topMenuButtons();
		if (buttons.length < 2) return;
		originalMoreButton = buttons[buttons.length - 1];
		var shortcutButton = originalMoreButton.cloneNode(false);
		shortcutButton.className = originalMoreButton.className + " image-editor-shortcuts";
		shortcutButton.textContent = localizedText("Keyboard Shortcuts", "Billentyűparancsok");
		shortcutButton.addEventListener(window.PointerEvent ? "pointerup" : "mouseup",
			function (event) {
				event.preventDefault();
				event.stopImmediatePropagation();
				shortcutPending = true;
				document.body.classList.add("image-editor-opening-menu");
				activateEditorControl(originalMoreButton);
				setTimeout(activateKeyboardShortcuts, 0);
			});
		originalMoreButton.parentElement.insertBefore(shortcutButton, originalMoreButton);
		hideControl(originalMoreButton);
	}

	function removeMenuItem(label) {
		var item = label.parentElement;
		if (!item) return;
		var previous = item.previousElementSibling;
		var next = item.nextElementSibling;
		item.remove();
		if (previous && previous.tagName === "HR" &&
			(!next || next.tagName === "HR")) previous.remove();
		else if (next && next.tagName === "HR" &&
			(!previous || previous.tagName === "HR")) next.remove();
	}

	function removeUnwantedMenuItems(root) {
		var labels = labelsIn(root);
		for (var i = labels.length - 1; i >= 0; i--) {
			var text = labels[i].textContent.trim();
			var normalized = normalizedLabel(text);
			if (hiddenSelectionCommands.has(normalized) ||
				windowRemovedLabels.has(normalized) ||
				matchesLocalized(text, "Subject", "Tárgy") ||
				matchesLocalized(text, "Local Storage", "Helyi meghajtó") ||
				matchesLocalized(text, "More", "Továbbiak") ||
				matchesLocalized(text, "Plugins", "Bővítmények"))
				removeMenuItem(labels[i]);
		}
	}

	function isEditMenu(panel) {
		var labels = labelsIn(panel);
		for (var i = 0; i < labels.length; i++) {
			if (matchesLocalized(labels[i].textContent,
				"Undo / Redo", "Visszavonás / Ismétlés")) return true;
		}
		return false;
	}

	function runOpenAndPlace() {
		openAndPlacePending = true;
		var buttons = topMenuButtons();
		if (!buttons.length) {
			openAndPlacePending = false;
			return;
		}
		activateEditorControl(buttons[0]);
		setTimeout(activateOpenAndPlace, 0);
	}

	function addImageImport(panel) {
		if (!isEditMenu(panel)) return;
		var installed = panel.querySelector(".image-editor-image-import");
		if (installed) {
			installed.querySelector(".label").textContent = imageImportText();
			return;
		}
		var item = document.createElement("div");
		item.className = "enab image-editor-image-import";
		item.innerHTML = '<span class="check"></span><span class="label"></span>';
		item.querySelector(".label").textContent = imageImportText();
		item.addEventListener(window.PointerEvent ? "pointerup" : "mouseup",
			function (event) {
				event.preventDefault();
				event.stopImmediatePropagation();
				runOpenAndPlace();
			});
		var separator = document.createElement("hr");
		separator.className = "image-editor-image-import-separator";
		panel.insertBefore(separator, panel.firstChild);
		panel.insertBefore(item, separator);
	}

	function activateOpenAndPlace() {
		if (!openAndPlacePending) return;
		var labels = labelsIn(document);
		for (var i = 0; i < labels.length; i++) {
			if (!matchesLocalized(labels[i].textContent,
				"Open & Place", "Megnyitás és elhelyezés")) continue;
			openAndPlacePending = false;
			activateMenuItem(labels[i].parentElement);
			return;
		}
	}

	function activateKeyboardShortcuts() {
		if (!shortcutPending) return;
		var labels = labelsIn(document);
		for (var i = 0; i < labels.length; i++) {
			if (!matchesLocalized(labels[i].textContent,
				"Keyboard Shortcuts", "Billentyűparancsok")) continue;
			shortcutPending = false;
			activateMenuItem(labels[i].parentElement);
			setTimeout(function () {
				document.body.classList.remove("image-editor-opening-menu");
			}, 150);
			return;
		}
		document.body.classList.remove("image-editor-opening-menu");
	}

	function customizeMenus() {
		var panels = document.querySelectorAll(".contextpanel");
		for (var i = 0; i < panels.length; i++) {
			addImageImport(panels[i]);
			removeUnwantedMenuItems(panels[i]);
		}
		activateOpenAndPlace();
		activateKeyboardShortcuts();
	}

	function installCanvasNavigation() {
		if (navigationInstalled) return;
		navigationInstalled = true;
		try {
			if (window.locStor) window.locStor.setItem("__zwh", "1");
			else window.localStorage.setItem("__zwh", "1");
		}
		catch (_) { }
		document.addEventListener("wheel", function (event) {
			var target = event.target;
			if (!target || !target.closest || !target.closest(".mainblock .body")) return;
			if (event.ctrlKey && !scaledWheelEvents.has(event) &&
				typeof WheelEvent === "function") {
				event.preventDefault();
				event.stopImmediatePropagation();
				var scaled = new WheelEvent("wheel", {
					bubbles: true, cancelable: true, composed: true,
					view: window, detail: event.detail,
					screenX: event.screenX, screenY: event.screenY,
					clientX: event.clientX, clientY: event.clientY,
					ctrlKey: true, shiftKey: event.shiftKey,
					altKey: event.altKey, metaKey: event.metaKey,
					deltaX: event.deltaX * 0.35,
					deltaY: event.deltaY * 0.35,
					deltaZ: event.deltaZ * 0.35,
					deltaMode: event.deltaMode
				});
				scaledWheelEvents.add(scaled);
				target.dispatchEvent(scaled);
			}
			else if (!event.ctrlKey) {
				event.preventDefault();
				event.stopImmediatePropagation();
			}
		}, {capture: true, passive: false});
	}

	function replaceVisibleBranding() {
		var walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
		var node;
		while ((node = walker.nextNode())) {
			if (!node.parentElement ||
				/^(SCRIPT|STYLE|NOSCRIPT)$/.test(node.parentElement.tagName)) continue;
			if (/photopea/i.test(node.nodeValue))
				node.nodeValue = node.nodeValue.replace(/photopea/gi, "ImageEditor");
		}
		var titled = document.querySelectorAll("[title]");
		for (var i = 0; i < titled.length; i++) {
			var title = titled[i].getAttribute("title");
			if (/photopea/i.test(title))
				titled[i].setAttribute("title",
					title.replace(/photopea/gi, "ImageEditor"));
		}
	}

	function notifyDocumentOpened() {
		var tabs = document.querySelectorAll(
			".mainblock > .block > .panelhead > div .label");
		if (tabs.length === documentCount) return;
		documentCount = tabs.length;
		if (!documentCount || window.parent === window) return;
		window.parent.postMessage(
			"image-editor-document-count:" + documentCount, "*");
	}

	var style = document.createElement("style");
	style.textContent =
		".mainblock > .block > .panelhead { display: none !important; }" +
		"html, body, .mainblock, .mainblock > .block, .mainblock > .block > .body {" +
		" border:0 !important; box-shadow:none !important; outline:0 !important; }" +
		".image-editor-shortcuts { white-space: nowrap; }" +
		".image-editor-opening-menu .contextpanel { visibility:hidden !important; }" +
		".confbar .body { box-sizing:border-box !important; padding-left:12px !important; }" +
		".confbar .body > :not(:first-child), .window .body .yesno {" +
		" margin-left:10px !important; padding-left:10px !important; }" +
		".window .body.flexrow > :last-child:not(:first-child) {" +
		" margin-left:18px !important; padding-left:12px !important; padding-right:12px !important; }" +
		".window .body > [style*='border-left'] {" +
		" box-sizing:border-box !important; padding-left:12px !important; padding-right:10px !important; }" +
		".window .body > :last-child { box-sizing:border-box; padding-right:10px; }" +
		".window .body > div > .flexrow > .form:last-child:not(:first-child) {" +
		" box-sizing:border-box !important; margin-left:18px !important; margin-right:12px !important;" +
		" padding-left:12px !important; padding-right:12px !important; }" +
		".window.afw_GEfc { box-sizing:border-box !important; max-height:100vh !important; max-width:100vw !important; }" +
		".window.afw_GEfc > .body { overflow:hidden !important; }" +
		".window.afw_GEfc > .body > div { box-sizing:border-box !important; max-width:100% !important; padding-right:0 !important; }" +
		".window.afw_GEfc > .body > div > .flexrow { box-sizing:border-box !important; max-width:100% !important; }" +
		".window.afw_GEfc > .body > div > .flexrow > button:first-of-type { display:none !important; }" +
		".window.afw_GEfc > .body > div > .flexrow > .form:last-child:not(:first-child) {" +
		" box-sizing:border-box !important; margin:0 10px !important; padding-left:12px !important; padding-right:12px !important; }" +
		"input[type='text'], input[type='number'], textarea, [contenteditable='true'] {" +
		" color: #d5d5d5 !important;" +
		" -webkit-text-fill-color: #d5d5d5 !important;" +
		" caret-color: #d5d5d5 !important;" +
		"}";
	document.head.appendChild(style);

	function scanDocument() {
		loadLocalization();
		hideTopBarItems();
		installShortcutButton();
		customizeMenus();
		installCanvasNavigation();
		replaceVisibleBranding();
		notifyDocumentOpened();
	}

	scanDocument();
	setInterval(scanDocument, 100);
})();

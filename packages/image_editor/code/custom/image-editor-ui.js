(function () {
	"use strict";

	var hiddenSelectionCommands = new Set([
		"Remove BG", "Subject", "Select Subject",
		"Quick Mask Mode", "Gyorsmaszk mód", "Gyorsmaszk módban",
		"Háttér eltávolítása", "Tárgy", "Tárgy kijelölése"
	]);
	var windowRemovedLabels = new Set([
		"More", "Plugins", "Továbbiak", "Bővítmények",
		"Local drive", "Local Drive", "Local storage", "Local Storage",
		"Helyi meghajtó"
	]);
	var localizedPhrases = new Map();
	var hardcodedPhraseSpecs = new Map([
		["Perspective Warp", {hu: "Perspektivikus torzítás", parts: ["Perspective", "Warp"]}],
		["Skew", {hu: "Nyírás", parts: ["Shear"]}],
		["Assign Profile", {hu: "Profil hozzárendelése", parts: ["Apply", "Profile"]}],
		["Convert to Profile", {hu: "Átalakítás profillá", parts: ["Convert to Shape", "Profile"]}],
		["Convert To Profile", {hu: "Átalakítás profillá", parts: ["Convert to Shape", "Profile"]}],
		["Wavelet Decompose", {hu: "Wavelet-felbontás", parts: ["Wave", "Separate"]}],
		["Text", {hu: "Szöveg", parts: ["Text"]}],
		["Scale Effects", {hu: "Effektusok méretezése", parts: ["Scale Effects"]}],
		["Current Path", {hu: "Aktuális görbe", parts: ["Current Path"]}],
		["Combine Shapes", {hu: "Alakzatok egyesítése", parts: ["Merge", "Shapes"]}],
		["Make Frames", {hu: "Képkockák létrehozása", parts: ["Create", "Frames"]}],
		["Unmake Frames", {hu: "Képkockák visszaalakítása", parts: ["Delete", "Frames"]}],
		["Defringe", {hu: "Perem eltávolítása", parts: ["Defringe"]}],
		["Grow", {hu: "Növelés", parts: ["Grow"]}],
		["Similar", {hu: "Hasonló", parts: ["Similar"]}],
		["Camera Raw", {hu: "Camera Raw", parts: ["Camera Raw"]}],
		["Vanishing Point", {hu: "Távlatpont", parts: ["Perspective", "Point"]}],
		["Normal Map", {hu: "Normáltérkép", parts: ["Normal Map"]}],
		["Texture Dilation", {hu: "Textúra kiterjesztése", parts: ["Texture", "Expand"]}],
		["Blur Gallery", {hu: "Életlenítési galéria", parts: ["Blur Gallery"]}],
		["Field Blur", {hu: "Mező életlenítése", parts: ["Field Blur"]}],
		["Iris Blur", {hu: "Írisz életlenítése", parts: ["Iris Blur"]}],
		["Tilt-Shift", {hu: "Tilt-shift", parts: ["Tilt-Shift"]}],
		["Path Blur", {hu: "Útvonal életlenítése", parts: ["Path Blur"]}],
		["Spin Blur", {hu: "Forgó életlenítés", parts: ["Spin Blur"]}],
		["Reduce Noise", {hu: "Zajcsökkentés", parts: ["Reduce Noise"]}],
		["Flame", {hu: "Láng", parts: ["Flame"]}],
		["Fibers", {hu: "Szálak", parts: ["Fibers"]}],
		["Solarize", {hu: "Szolarizálás", parts: ["Solarize"]}],
		["Trace Contour", {hu: "Kontúr követése", parts: ["Trace Contour"]}],
		["Wind", {hu: "Szél", parts: ["Wind"]}],
		["Color to Alpha", {hu: "Szín alfává", parts: ["Color to Alpha"]}],
		["Particles", {hu: "Részecskék", parts: ["Particles"]}],
		["Fourier", {hu: "Fourier", parts: ["Fourier"]}],
		["Fourier Transform", {hu: "Fourier-transzformáció", parts: ["Fourier", "Transform"]}],
		["Inverse Fourier Transform", {hu: "Inverz Fourier-transzformáció", parts: ["Inverse", "Fourier", "Transform"]}],
		["Character Styles", {hu: "Karakterstílusok", parts: ["Character", "Styles"]}],
		["Vector Info", {hu: "Vektorinformáció", parts: ["Vector Mask", "Info"]}],
		["Main Menu", {hu: "Főmenü", parts: ["More", "Menu"]}],
		["Navigation", {hu: "Navigáció", parts: ["Navigator"]}],
		["Vertical scroll", {hu: "Függőleges görgetés", parts: ["Vertical", "Move"]}],
		["Horizontal scroll", {hu: "Vízszintes görgetés", parts: ["Horizontal", "Move"]}],
		["Wheel", {hu: "Egér görgő", parts: ["Move"]}],
		["Zooming", {hu: "Nagyítás", parts: ["Zoom In"]}],
		["Quick tools (press to enable, release to disable)", {hu: "Gyorseszközök (lenyomva engedélyezés, felengedve kikapcsolás)", parts: ["Quick Selection", "Tools", "Enable", "Disable"]}],
		["Tools", {hu: "Eszközök", parts: ["Tools"]}],
		["Decrease Brush Size", {hu: "Ecsetméret csökkentése", parts: ["Decrease", "Brush", "Size"]}],
		["Increase Brush Size", {hu: "Ecsetméret növelése", parts: ["Increase", "Brush", "Size"]}],
		["Decrease Hardness", {hu: "Keménység csökkentése", parts: ["Decrease", "Hardness"]}],
		["Increase Hardness", {hu: "Keménység növelése", parts: ["Increase", "Hardness"]}],
		["Color Sampler", {hu: "Színmintavevő", parts: ["Color Sampler"]}],
		["Magic Eraser", {hu: "Varázsradír", parts: ["Magic Eraser"]}],
		["Add Anchor Point", {hu: "Rögzítési pont hozzáadása", parts: ["Add", "Anchor", "Point"]}],
		["Delete Anchor Point", {hu: "Rögzítési pont törlése", parts: ["Delete", "Anchor", "Point"]}],
		["Convert Point", {hu: "Pont átalakítása", parts: ["Transform", "Point"]}],
		["New Smart Obj. via Copy", {hu: "Új intelligens objektum másolással", parts: ["New", "Smart Object", "Copy"]}],
		["Open (Edit Contents)", {hu: "Megnyitás (tartalom szerkesztése)", parts: ["Open", "Edit", "Content Aware"]}],
		["Edit Contents", {hu: "Tartalom szerkesztése", parts: ["Edit", "Content Aware"]}],
		["Reset Transform", {hu: "Transzformáció visszaállítása", parts: ["Reset", "Transform"]}],
		["Replace Contents", {hu: "Tartalom cseréje", parts: ["Replace", "Content Aware"]}],
		["Export Contents", {hu: "Tartalom exportálása", parts: ["Export Layers", "Content Aware"]}],
		["Convert to Layers", {hu: "Átalakítás rétegekké", parts: ["Convert to Shape", "Layers"]}],
		["Convert To Layers", {hu: "Átalakítás rétegekké", parts: ["Convert to Shape", "Layers"]}],
		["Turn into JPG", {hu: "Átalakítás JPG-vé", parts: ["Convert to Shape", "JPG"]}],
		["Standard Deviation", {hu: "Szórás", parts: ["Standard", "Distribution"]}],
		["Summation", {hu: "Összegzés", parts: ["Add"]}],
		["Variance", {hu: "Variancia", parts: ["Distribution"]}]
	]);
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

	function localizedHardcodedText(english) {
		var specification = hardcodedPhraseSpecs.get(english);
		if (!specification) return null;
		if (language().indexOf("hu") === 0) return specification.hu;
		var direct = localizedPhrases.get(english);
		if (direct && direct !== english) return direct;
		if (!specification.parts || language().indexOf("en") === 0) return english;
		return specification.parts.map(function (part) {
			var translated = localizedPhrases.get(part);
			return translated && translated !== part ? translated : part;
		}).join(" ");
	}

	function translatedUiValue(value) {
		var source = String(value || "");
		var leading = source.match(/^\s*/)[0];
		var trailing = source.match(/\s*$/)[0];
		var core = source.slice(leading.length, source.length - trailing.length);
		var ellipsis = "";
		var ellipsisMatch = core.match(/(\.{3}|…)$/);
		if (ellipsisMatch) {
			ellipsis = ellipsisMatch[1];
			core = core.slice(0, -ellipsis.length).trimEnd();
		}
		var translated = localizedHardcodedText(core);
		if (!translated) {
			var shortcut = core.match(/^(.*?)(\s+\([^()]{1,12}\))$/);
			if (shortcut) {
				translated = localizedHardcodedText(shortcut[1]);
				if (translated) translated += shortcut[2];
			}
		}
		if (!translated && /(^|\s)Wheel$/.test(core)) {
			var wheel = localizedHardcodedText("Wheel");
			if (wheel) translated = core.slice(0, -5) + wheel;
		}
		return translated ? leading + translated + ellipsis + trailing : source;
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

	function isViewMenu(panel) {
		var labels = labelsIn(panel);
		for (var i = 0; i < labels.length; i++) {
			if (matchesLocalized(labels[i].textContent, "Zoom In", "Nagyítás") ||
				matchesLocalized(labels[i].textContent, "Rulers", "Vonalzók"))
				return true;
		}
		return false;
	}

	function removeViewMode(panel) {
		if (!isViewMenu(panel)) return;
		var labels = labelsIn(panel);
		for (var i = labels.length - 1; i >= 0; i--) {
			if (matchesLocalized(labels[i].textContent, "Mode", "Mód"))
				removeMenuItem(labels[i]);
		}
	}

	function localizeHardcodedUi() {
		var roots = document.querySelectorAll(".contextpanel, .window, .toolbar, .tools");
		for (var i = 0; i < roots.length; i++) {
			var walker = document.createTreeWalker(roots[i], NodeFilter.SHOW_TEXT);
			var node;
			while ((node = walker.nextNode())) {
				var translated = translatedUiValue(node.nodeValue);
				if (translated !== node.nodeValue) node.nodeValue = translated;
			}
		}
		var titled = document.querySelectorAll("[title], [aria-label]");
		for (var j = 0; j < titled.length; j++) {
			for (var k = 0; k < 2; k++) {
				var attribute = k ? "aria-label" : "title";
				if (!titled[j].hasAttribute(attribute)) continue;
				var value = titled[j].getAttribute(attribute);
				var translatedValue = translatedUiValue(value);
				if (translatedValue !== value)
					titled[j].setAttribute(attribute, translatedValue);
			}
		}
	}

	function hideRemovedToolbarControls() {
		var controls = document.querySelectorAll("[title], [aria-label]");
		for (var i = 0; i < controls.length; i++) {
			if (controls[i].closest(".contextpanel")) continue;
			var text = controls[i].getAttribute("title") ||
				controls[i].getAttribute("aria-label") || "";
			if (matchesLocalized(text, "Quick Mask Mode", "Gyorsmaszk mód") ||
				normalizedLabel(text) === "Virtual Keys")
				hideControl(controls[i]);
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
			removeViewMode(panels[i]);
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
		localizeHardcodedUi();
		hideTopBarItems();
		hideRemovedToolbarControls();
		installShortcutButton();
		customizeMenus();
		installCanvasNavigation();
		replaceVisibleBranding();
		notifyDocumentOpened();
	}

	scanDocument();
	setInterval(scanDocument, 100);
})();

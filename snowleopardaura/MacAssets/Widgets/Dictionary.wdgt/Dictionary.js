/*
Copyright © 2007, Apple Computer, Inc.  All rights reserved.
NOTE:  Use of this source code is subject to the terms of the Software
License Agreement for Mac OS X, which accompanies the code.  Your use
of this source code signifies your agreement to such license terms and
conditions.  Except as expressly granted in the Software License Agreement
for Mac OS X, no other copyright, patent, or other intellectual property
license or right is granted, either expressly or by implication, by Apple.
*/

var kCollapsedWindowWidth    = 381;
var kCollapsedWindowHeight   = 49;
var kMinExpandedWindowWidth  = kCollapsedWindowWidth;
var kMinExpandedWindowHeight = 228;
var kMinExpandedWindowRect   = new AppleRect(0, 0, kMinExpandedWindowWidth, kMinExpandedWindowHeight);
var kDefaultWindowWidth  	 = kMinExpandedWindowWidth;
var kDefaultWindowHeight 	 = kMinExpandedWindowHeight;
var kWindowWidthKey  		 = "windowWidth";
var kWindowHeightKey 		 = "windowHeight";

var kResizeAnimationDuration = 500;
var kResizeAnimationInterval = 10;

var kStateCollapsed = 0;
var kStateResizing  = 1;
var kStateExpanded  = 2;
var gCurrentState 	= kStateCollapsed;

var kDictionaryTypeDictionary = 0;
var kDictionaryTypeThesaurus  = 1;
var kDefaultDictionaryType	  = kDictionaryTypeDictionary;
var kDictionaryTypeKey 		  = "dictionaryType";
var gCurrentDictionaryType	  = kDictionaryTypeDictionary;

var kViewTypeDefinition 	= 0;
var kViewTypeIndex      	= 1;
var gCurrentViewType		= kViewTypeDefinition;

var gCurrentHeadwordIndex 	= -1;

var kFontSizeKey 		= "fontSize";
var kDefaultFontSize 	= "medium";

var gScrollArea;
var gScrollbar;

var gCurrentSearchCommand;
var gCurrentHeadwords;
var gHistoryAddTimeoutId;

if (window.widget) {
	widget.onshow = onshow;
	widget.onsync = onsync;
	widget.onremove = onremove;
}

function onshow() {
	selectAllInSearchField();
}

function onsync() {
	renderFontSizePreference();
}

function onremove() {
	setInstancePreferenceForKey(null, kWindowWidthKey);
	setInstancePreferenceForKey(null, kWindowHeightKey);
	setInstancePreferenceForKey(null, kFontSizeKey);
}

function getLocalizedString(string) {
	try { string = localizedStrings[string] || string; } catch (e) {}
	return string;
}

function $(id) {
	return document.getElementById(id);
}

function createKey(key) {
	if (window.widget) {
        key = widget.identifier + "-" + key;
	}
	return key;
}

function setInstanceAndGlobalPreferenceForKey(value, key) {
	setInstancePreferenceForKey(value, key);
	setGlobalPreferenceForKey(value, key);
}

function setInstancePreferenceForKey(value, key) {
	setGlobalPreferenceForKey(value, createKey(key));
}

function setGlobalPreferenceForKey(value, key) {
	if (window.widget) {
		widget.setPreferenceForKey(value, key);
	}
}

function preferenceForKey(key) {
	var result;
	if (window.widget) {
		result = widget.preferenceForKey(createKey(key));
		if (!result) {
			result = widget.preferenceForKey(key);
		}
	}
	if (!result) {
		result = eval("kDefault" + key.substring(0,1).toUpperCase() + key.substring(1));
	}
	return result;
}

function setupDataScrollbar() {
	if (!gScrollArea) {
		gScrollArea = new AppleScrollArea($("dataDiv"));
		gScrollbar  = new AppleVerticalScrollbar($("dataScroll"));
		gScrollArea.addScrollbar(gScrollbar);
	}
}

function refreshDataScrollbar() {
	gScrollArea.refresh();
	gScrollbar.refresh();
}

function load() {
	setupActiveDictionaries();
	gCurrentDictionaryType = preferenceForKey(kDictionaryTypeKey);
	updateDictionaryPopUp();
	renderFontSizePreference();
	selectAllInSearchField();
	new AppleInfoButton($("infoButton"), $("front"), "black", "black", doShowBackResizeAnimation);
	new AppleGlassButton($("doneButton"), getLocalizedString("Done"), showFront);
	
	fetchUiStrings();
	setupDataScrollbar();
	preloadImages();
	loadPreferences();
	
	// scroll focus
	$("searchField").addEventListener("mousedown", searchFieldMouseDownBubblePhase, false);
	$("dataDiv").addEventListener("mousedown", dataDivMouseDownBubblePhase, false);
}

function searchFieldMouseDownBubblePhase(evt) {
	gScrollArea.blur();
}

function dataDivMouseDownBubblePhase(evt) {
	gScrollArea.focus();
	$("searchField").blur();
}

function loadPreferences() {
	var fontSize = preferenceForKey(kFontSizeKey);
	setInstanceAndGlobalPreferenceForKey(fontSize, kFontSizeKey);
	var windowWidth = preferenceForKey(kWindowWidthKey);
	setInstanceAndGlobalPreferenceForKey(windowWidth, kWindowWidthKey);
	var windowHeight = preferenceForKey(kWindowHeightKey);
	setInstanceAndGlobalPreferenceForKey(windowHeight, kWindowHeightKey);	
}

function setupActiveDictionaries() {
	var dicts = [];
	if (window.DictionaryPlugin) {
		dicts = DictionaryPlugin.getActiveDictionaries();
	}
	var selectEl = $("dictSelect");
	var optionEl;
	for (var i = 0; i < dicts.length; i++) {
		optionEl = document.createElement("option");
		optionEl.setAttribute("value", i);
		optionEl.innerText = dicts[i];
		selectEl.appendChild(optionEl);
	}
}

function fetchUiStrings() {
	$("fontSizeLabel")	.innerText = getLocalizedString("Font size:");
	$("small")			.innerText = getLocalizedString("Small");
	$("medium")			.innerText = getLocalizedString("Medium");
	$("large")			.innerText = getLocalizedString("Large");
}

function preloadImages() {
	var names = ["left", "left_p", "right", "right_p", "thumb", "thumb pressed"];
	for (var i = 0; i < names.length; i++) {
		new Image(10, 13).src = "Images/" + names[i] + ".png";
	}
}

function doShowBackResizeAnimation(evt) {
	storeScrollPosInLastSearchCommand();
	var expanding = false;
	doResizeAnimation(expanding, showBack, kMinExpandedWindowRect);
}

function showBack() {
	if (window.widget) {
		widget.prepareForTransition("ToBack");
	}
	ElementUtils.hide($("front"));
	ElementUtils.show($("back"));
	if (window.widget) {
		setTimeout("widget.performTransition();", 0);
	}
}

function showFront(evt) {
	if (window.widget) {
		widget.prepareForTransition("ToFront");
	}
	ElementUtils.show($("front"));
	ElementUtils.hide($("back"));
	front.className = "resizing";
	if (window.widget) {
		setTimeout("widget.performTransition();", 0);
	}
	setTimeout("doShowFrontResizeAnimation();", 700);
}

function doShowFrontResizeAnimation() {
	var expanding = true;
	doResizeAnimation(true, giggleTheHandle, kMinExpandedWindowRect);
}

// this kludge seems to be necessary to force the front of the widget to redraw after showFront()
function giggleTheHandle() {
	refreshDataScrollbar();
	if (gCurrentSearchCommand) {
		gScrollArea.verticalScrollTo(gCurrentSearchCommand.scrollPos);
	}
	if (window.widget) {
		var x = window.screenX;
		var y = window.screenY;
		var w = window.innerWidth;
		var h = window.innerHeight;
		widget.resizeAndMoveTo(x, y, w, h+1);	
		widget.resizeAndMoveTo(x, y, w, h);	
	}
}

function trimLeadingWhiteSpace(str) {
	return str.replace(/^\s*/g, "");
}

function consumeEvent(evt) {
	if (evt) {
		evt.stopPropagation();
		evt.preventDefault();
	}
}

var ElementUtils = {};

ElementUtils.resizeTo = function(el, w, h) {
	el.style.width  = parseInt(w) + "px";
	el.style.height = parseInt(h) + "px";
};

ElementUtils.hide = function(el) {
	el.style.display = "none";
};

ElementUtils.show = function(el) {
	el.style.display = "block";
};

ElementUtils.getOffsetTop = function(el, outermostAncestor) {
	outermostAncestor = (outermostAncestor) ? outermostAncestor : document.documentElement;
	var result = 0;
	do {
		result += el.offsetTop;
		if (el == outermostAncestor) {
			break;
		}
	} while (el = el.offsetParent);
	return result;
};

var WidgetUtils = {};

WidgetUtils.resizeTo = function(w, h) {
	w = (-1 == w) ? window.innerWidth  : w;
	h = (-1 == h) ? window.innerHeight : h;
	ElementUtils.resizeTo($("front"), w, h);
	if (window.widget) {
		window.resizeTo(parseInt(w), parseInt(h));
	}
};

function selectAllInSearchField() {
	var searchField = $("searchField");
	searchField.focus();
	searchField.select();
}

function resizerMouseDown(evt) {
	document.addEventListener("mousemove", resizerMouseMove, false);
	document.addEventListener("mouseup", resizerMouseUp, false);	

	var frontStyle  = document.defaultView.getComputedStyle($("front"), "");
	var frontWidth  = parseInt(frontStyle.width);
	var frontHeight = parseInt(frontStyle.height);
	resizerMouseMove.deltaX = frontWidth  - evt.pageX;
	resizerMouseMove.deltaY = frontHeight - evt.pageY;
	consumeEvent(evt);
}

function resizerMouseMove(evt) {
	var w = evt.pageX + resizerMouseMove.deltaX;
	var h = evt.pageY + resizerMouseMove.deltaY;
	w = (w < kMinExpandedWindowWidth)  ? kMinExpandedWindowWidth  : w;
	h = (h < kMinExpandedWindowHeight) ? kMinExpandedWindowHeight : h;
	WidgetUtils.resizeTo(w, h);
	refreshDataScrollbar();
	consumeEvent(evt);
}

function resizerMouseUp(evt) {
	document.removeEventListener("mousemove", resizerMouseMove, false);
	document.removeEventListener("mouseup", resizerMouseUp, false);
	setInstancePreferenceForKey(window.innerWidth, kWindowWidthKey);
	setInstancePreferenceForKey(window.innerHeight, kWindowHeightKey);
	refreshDataScrollbar();
	consumeEvent(evt);
}

function searchFieldKeyPressed(evt) {
	var keyCode = evt.keyCode;
	if (13 == keyCode || 3 == keyCode) { // return or enter key
		searchFieldSearched.wasExecuted = true;
	} else {
		searchFieldSearched.wasExecuted = false;
		if (27 == keyCode) { // ESC key
			searchFieldCleared(evt);
		}
	}
	searchFieldSearched.wasKeyPressed = true;
}

searchFieldSearched.wasExecuted = false;
searchFieldSearched.wasKeyPressed = false;

function searchFieldSearched(evt) {
	if (!searchFieldSearched.wasKeyPressed) { // the 'x' must have been clicked
		searchFieldCleared(evt);
	} else {
		var delayHistory = !searchFieldSearched.wasExecuted;
		var func = function() {search(delayHistory)};
		//if (searchFieldSearched.wasExecuted) {
			gCurrentViewType = kViewTypeDefinition;
		//}
		if (gCurrentState == kStateCollapsed && searchFieldSearched.wasExecuted) {
			var expanding = true;
			doResizeAnimation(expanding, func);
		} else if (gCurrentState == kStateExpanded) {
			func();
		}
	}
	searchFieldSearched.wasKeyPressed = false;
}

function searchFieldCleared(evt) {
	var expanding = false;
	doResizeAnimation(expanding);
	$("definitionViewDiv").innerHTML = "";
	$("indexViewDiv").innerHTML = "";
	$("indexCharDiv").innerText = "";
}

function doResizeAnimation(expanding, callback, altSmallRect) {
	var front = $("front");
	if (expanding) {
		if ("expanded" == front.className) {
			return;
		}
	} else {
		if ("expanded" != front.className) {
			return;
		}
		ElementUtils.hide($("paperContent"));
		refreshDataScrollbar();
	}
	gCurrentState = kStateResizing;	
	front.className = "resizing";

	var animator = new AppleAnimator(kResizeAnimationDuration, kResizeAnimationInterval);
	
	var smallRect = new AppleRect(0, 0, (expanding ? window.innerWidth : kCollapsedWindowWidth), kCollapsedWindowHeight);
	var largeRect = new AppleRect(0, 0, parseInt(preferenceForKey(kWindowWidthKey)), parseInt(preferenceForKey(kWindowHeightKey)));

	var animation = new AppleRectAnimation(
		(expanding ? (altSmallRect ? altSmallRect : smallRect) : largeRect),
		(expanding ? largeRect : (altSmallRect ? altSmallRect : smallRect)),
		resizeAnimationHandler);
		
	animator.addAnimation(animation);
	animator.oncomplete = function(){resizingAnimationCompleteHandler(expanding, callback)};
	animator.start();
}

function resizeAnimationHandler(animator, currRect, startRect, finRect) {
	WidgetUtils.resizeTo(currRect.right, currRect.bottom);
}

function resizingAnimationCompleteHandler(expanding, callback) {
	if (expanding) {
		$("front").className = "expanded";
		gCurrentState = kStateExpanded;
		ElementUtils.show($("paperContent"));
		setupDataScrollbar();
		refreshDataScrollbar();
	} else {
		$("front").className = "collapsed";
		gCurrentState = kStateCollapsed;
	}
	if (callback) {
		callback();
	}
}

function addSearchToHistory(delay) {
	var millis = (delay) ? 1000 : 0;
	clearTimeout(gHistoryAddTimeoutId);
	gHistoryAddTimeoutId = setTimeout("gHistory.add(gCurrentSearchCommand); updateHistoryButtons();", millis);
}

function search(delayHistory) {
	var searchString = trimLeadingWhiteSpace($("searchField").value);
	// if search string is null, empty, or just whitespace, hide view and don't search
	if (!searchString || !searchString.length || 0 == searchString.search(/^\s*$/gi)) {
		ElementUtils.hide($("definitionViewDiv"));
		ElementUtils.hide($("indexViewDiv"));
		return;
	}
	
	storeScrollPosInLastSearchCommand();
	var cmd = new SearchCommand(gCurrentHeadwordIndex, gCurrentDictionaryType, searchString, gCurrentViewType);
	doSearch(cmd);
	gCurrentHeadwordIndex = -1;
	clearTimeout(gHistoryAddTimeoutId);
	addSearchToHistory(delayHistory);
}

function doSearch(searchCommand) {
	hideHeadwordHighlight();
	var searchString = searchCommand.searchString;
	var searchField = $("searchField");
	$("indexCharDiv").innerText = searchString.substring(0,1).toUpperCase();

	gCurrentSearchCommand = searchCommand;
	gCurrentDictionaryType = searchCommand.dictionaryType;
	gCurrentViewType = searchCommand.viewType;
	updateDictionaryPopUp();

	var defView = $("definitionViewDiv");
	var indexView = $("indexViewDiv");

	ElementUtils.hide(defView);
	ElementUtils.hide(indexView);

	var results;
	var resultDoc;
	var headwords = [];
	if (window.DictionaryPlugin) {
		if (searchCommand.headwordIndex > -1) {
			results = DictionaryPlugin.doSearchForHeadwordAtIndex(searchCommand.headwordIndex);
		} else {
			results = DictionaryPlugin.doSearchForString(searchString);
		}
		gCurrentHeadwords = DictionaryPlugin.currentHeadwords();
		
		// note: plugin returns a complete HTML document as the result including html, head, and body elements.
		// we extract any stylesheet info and attach to widget as a whole and then extract body contents
		// and place in widget ui scrollarea.
		if(results && results.length)
		{
			resultDoc = domDocumentForXMLString(results[0]);
			var href = getCSSStyleSheetHrefFromDoc(resultDoc);
			if (href) {
				$("currentDictStyleSheet").href = href;
			}
			
		}
	}


	if (!(results && results.length)) {
		results = [getLocalizedString('"%@" could not be found.').replace("%@", searchString)];
		gCurrentHeadwords = null;
	}
	
	resultDoc = null;			
	
	var completeResultString = "";
	for(var i = 0; i < results.length; i++)
		completeResultString += results[i].replace(/id=\"\w*\"/, "");

	defView.innerHTML = completeResultString;
	var allElements = defView.querySelectorAll("*");
	for(var i = 0; i < allElements.length; i++)
	{

		for(var j = 0; j < allElements[i].childNodes.length; j++){
			
			if(allElements[i].childNodes[j].nodeType == 3)
			{
				allElements[i].style.appleDashboardRegion = "dashboard-region(control rectangle)";
				allElements[i].style["-khtml-user-select"] = 'text';
				allElements[i].style.cursor = 'text';
				break;
			}			
		}
	}	
	
	if (gCurrentViewType == kViewTypeDefinition) {
		ElementUtils.show(defView);
		enableDictionaryStyleSheet();
	} else {
		populateIndexViewTable();
		ElementUtils.show(indexView);
		disableDictionaryStyleSheet();
	}
	if (searchFieldSearched.wasExecuted) {
		selectAllInSearchField();
	} else {
		searchField.focus();
	}
	gScrollArea.verticalScrollTo(searchCommand.scrollPos);
	refreshDataScrollbar();
}

function domDocumentForXMLString(str) {
	var parser = new DOMParser();
	return parser.parseFromString(str, "text/xml");
}

function getCSSStyleSheetHrefFromDoc(doc) {
	var linkEls = doc.getElementsByTagName("link");
	if (linkEls && linkEls.length) {
		var linkEl = linkEls.item(0);
		return linkEl.getAttribute("href");
	}
	return null;
}

function htmlStringForCurrentHeadwords() {
	var result = "<table cellspacing='0'>";
	if (window.DictionaryPlugin) {
		var headword, index, def;
		var defs = DictionaryPlugin.simpleDefsForCurrentHeadwords();
		for (var i = 0; i < defs.length; i++) {
			headword = gCurrentHeadwords[i];
			def = defs[i];
			result += "<tr><th onmouseover='headwordMouseOver(event, "+i+");'>"
				+ headword + "</th><td>" + def + "</td></tr>";
		}
	}
	result += "</table>";
	return result;
}

function updateDictionaryPopUp() {
	$("dictSelect").options.selectedIndex = gCurrentDictionaryType;
	updateDictionaryLogo();
}

function disableDictionaryStyleSheet() {
	$("currentDictStyleSheet").disabled = true;
}

function enableDictionaryStyleSheet() {
	$("currentDictStyleSheet").disabled = false;
}

function dictionaryPopUpChanged(evt) {
	gCurrentDictionaryType = $("dictSelect").options.selectedIndex;
	DictionaryPlugin.setActiveDictionary(gCurrentDictionaryType);	

	updateDictionaryLogo();
	if (gCurrentState == kStateExpanded) {
		gCurrentViewType = kDictionaryTypeDictionary;
		search();
	}
}

var gHistory = new History();

function History() {
	this.stack = [];
	this.index = -1;
}

History.prototype.add = function(item) {
	this.stack.length = ++this.index;
	this.stack[this.index] = item;
};

History.prototype.back = function() {
	if (this.canGoBack()) {
		return this.stack[--this.index];
	}
};

History.prototype.fwd = function() {
	if (this.canGoFwd()) {
		return this.stack[++this.index];
	}
};

History.prototype.canGoBack = function() {
	if (this.index == 0 || this.stack.length == 0) {
		return false;
	}
	return this.index > -1;
};

History.prototype.canGoFwd = function() {
	return this.stack.length > 0 && this.index < this.stack.length - 1;
};

function SearchCommand(headwordIndex, dictionaryType, searchString, viewType) {
	this.headwordIndex = headwordIndex;
	this.dictionaryType = dictionaryType;
	this.searchString = searchString;
	this.viewType = viewType;
	this.scrollPos = 0;
}

function historyButtonMouseDown(evt) {
	var isBack = ("backButton" == evt.target.id);
	if ((isBack && !gHistory.canGoBack()) || (!isBack && !gHistory.canGoFwd())) {
		return;
	}
	var func = function() {doHistoryStep(isBack)};
	if (gCurrentState == kStateCollapsed) {
		var expanding = true;
		doResizeAnimation(expanding, func);
	} else {
		func();
	}
}

function doHistoryStep(isBack) {
	var canGo = (isBack ? gHistory.canGoBack() : gHistory.canGoFwd());
	if (canGo) {
		storeScrollPosInLastSearchCommand();
		hideHeadwordHighlight();
		var el = (isBack ? $("backWrap") : $("fwdWrap"));
		el.className = "pressed";
		var cmd = (isBack ? gHistory.back() : gHistory.fwd());
		$("searchField").value = cmd.searchString;
		doSearch(cmd);
	}
}

function updateHistoryButtons() {
	$("backWrap").className = (gHistory.canGoBack() ? "enabled" : "");
	$("fwdWrap").className  = (gHistory.canGoFwd()  ? "enabled" : "");
}

function storeScrollPosInLastSearchCommand() {
	if (gCurrentSearchCommand) {
		gCurrentSearchCommand.scrollPos = gScrollArea.content.scrollTop;
	}
}

function viewSelectButtonClicked(evt) {
	
	//Don't switch to index view or add to history unless there are headwords
	if (gCurrentViewType == kViewTypeIndex || gCurrentHeadwords)
	{
		gCurrentViewType = (gCurrentViewType == kViewTypeDefinition ?  kViewTypeIndex : kViewTypeDefinition);

		storeScrollPosInLastSearchCommand();
		var searchString = trimLeadingWhiteSpace($("searchField").value);
		gCurrentSearchCommand = new SearchCommand(gCurrentHeadwordIndex, gCurrentDictionaryType, searchString, gCurrentViewType);

		addSearchToHistory();

		if (gCurrentViewType == kViewTypeDefinition) {
			switchedToDefinitionView();
		} else {
			switchedToIndexView();
		}
	}
	gScrollArea.focus();
}

function switchedToDefinitionView() {
	ElementUtils.hide($("indexViewDiv"));
	ElementUtils.show($("definitionViewDiv"));
	hideHeadwordHighlight();
	
	gScrollArea.verticalScrollTo(0);
	refreshDataScrollbar();	
	enableDictionaryStyleSheet();
}

function switchedToIndexView() {
	populateIndexViewTable();

	ElementUtils.hide($("definitionViewDiv"));
	ElementUtils.show($("indexViewDiv"));

	gScrollArea.verticalScrollTo(0);
	refreshDataScrollbar();
	disableDictionaryStyleSheet();
}

function populateIndexViewTable() {
	if (gCurrentViewType == kViewTypeIndex) {
		var html = (gCurrentHeadwords ? htmlStringForCurrentHeadwords() : "");
		$("indexViewDiv").innerHTML = html;
	}
}

function headwordMouseOver(evt, i) {
	gCurrentHeadwordIndex = i;
	var th = evt.target;
	var headword = th.innerText;
	var offsetTop = ElementUtils.getOffsetTop(th, $("dataDiv"));
	var div = $("headwordHighlight");
	ElementUtils.show(div);
	div.innerText = headword;
	div.style.top = offsetTop + "px";
	consumeEvent(evt);
}

function indexViewMouseOver(evt) {
	hideHeadwordHighlight();
}

function hideHeadwordHighlight() {
	var div = $("headwordHighlight");
	div.innerText = "";
	ElementUtils.hide(div);
}

function headwordClicked(evt) {
	var searchString;
	if (window.DictionaryPlugin) {
		// problem here is that you want to ask Plugin for search string for the headword in Japanese,
		// but you DONT for english!!! 電話
		searchString = DictionaryPlugin.searchStringForHeadwordAtIndex(gCurrentHeadwordIndex);
	}
	if (!searchString) {
		searchString = $("headwordHighlight").innerText;
	}
	$("searchField").value = searchString;
	hideHeadwordHighlight();
	gCurrentViewType = kViewTypeDefinition;
	search();	
}

function fontSizeChanged(evt) {
	var el = evt.target;
	var newSize = el.options[el.options.selectedIndex].value;
	setInstancePreferenceForKey(newSize, kFontSizeKey);
	setGlobalPreferenceForKey(newSize, kFontSizeKey);
	renderFontSizePreference();
	gScrollArea.verticalScrollTo(0);
	storeScrollPosInLastSearchCommand();
}

function renderFontSizePreference() {
	var newSize = preferenceForKey(kFontSizeKey);
	$("dataDiv").className = newSize;
	$("fontSizeSelect").value = newSize;
}

var dictionaryProviders = {
	"Oxford": {logo_src: "Images/oxford_logo.png", url: "http://www.oup.com/us", alt: "Oxford American Dictionaries"},
	"Shogakukan": {logo_src: "Images/shogakukan_logo.png", url: "http://www.shogakukan.co.jp", alt: "Shogakukan Logo"}
};
var currentDictionaryProvider = dictionaryProviders["Oxford"];

function updateDictionaryLogo()
{
	var identifier = DictionaryPlugin.getSelectedDictionaryIdentifier();
	alert(identifier)
	if(identifier == "com.apple.dictionary.Daijisen" || identifier == "com.apple.dictionary.ruigo" || identifier == "com.apple.dictionary.PEJ-PJE")
		currentDictionaryProvider = dictionaryProviders["Shogakukan"];
	else if(identifier == "com.apple.dictionary.OAWT" || identifier == "com.apple.dictionary.NOAD")
		currentDictionaryProvider = dictionaryProviders["Oxford"];
	//if it's an apple dictionary, keep same logo

	if(currentDictionaryProvider)
	{
		$('providerLogo').src = currentDictionaryProvider["logo_src"];
		$('providerLogo').alt = currentDictionaryProvider["alt"];
		$('providerLogo').onclick = function() { widget.openURL(currentDictionaryProvider["url"]); }		
	}
}

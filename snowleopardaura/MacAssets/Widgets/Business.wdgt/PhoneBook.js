/*
Copyright (c) 2005, Apple Computer, Inc.  All rights reserved.
NOTE: Use of this source code is subject to the terms of the Software
License Agreement for Mac OS X, which accompanies the code.  Your use
of this source code signifies your agreement to such license terms and
conditions.  Except as expressly granted in the Software License Agreement
for Mac OS X, no other copyright, patent, or other intellectual property
license or right is granted, either expressly or by implication, by Apple.
*/
var kCollapsedWindowHeight   = 51;
var kMinExpandedWindowHeight = 240;
var kMaxExpandedWindowHeight = 505;
var kDefaultWindowWidth  	 = 335;
var kMinExpandedWindowRect   = new AppleRect(0, 0, kDefaultWindowWidth, kMinExpandedWindowHeight);

var kDefaultWindowHeight 	 = kMinExpandedWindowHeight;
var kWindowHeightKey 		 = "windowHeight";
var kDefaultLocation		 = "95014";
var kLocationKey	 		 = "customAddress";
var kDefaultSearchRadius 	 = 20;
var kSearchRadiusKey 		 = "searchRadius";
var kDefaultItemsPerPage 	 = 10;
var kItemsPerPageKey 		 = "itemsPerPage";

var gDefaultPreferencesTable = {};
gDefaultPreferencesTable[kWindowHeightKey] 	= kDefaultWindowHeight;
gDefaultPreferencesTable[kLocationKey] 		= kDefaultLocation;
gDefaultPreferencesTable[kSearchRadiusKey] 	= kDefaultSearchRadius;
gDefaultPreferencesTable[kItemsPerPageKey] 	= kDefaultItemsPerPage;

var gCurrentMaxExpandedWindowHeight = kMaxExpandedWindowHeight;

var kResizeAnimationDuration = 500;
var kResizeAnimationInterval = 10;

var kStateCollapsed = 0;
var kStateResizing  = 1;
var kStateExpanded  = 2;
var gCurrentState 	= kStateCollapsed;
var gShouldSearchWhenFrontShown = false;
var gBusy = false;

var gCurrentBizId;
var gSearchCommand;

var gResultsListScrollArea;
var gProgressIndicator;
var gBrowser;
var gXstlDocument;

var gCategories = [
	{code:"602101", title:"Banks"},
	{code:"724101", title:"Barbers"},
	{code:"723106", title:"Beauty Salons"},
	{code:"721201", title:"Cleaners"},
	{code:"581228", title:"Coffee Shops"},
	{code:"531102", title:"Department Stores"},
	{code:"599201", title:"Florists-Retail"},
	{code:"554101", title:"Gasoline & Oil-Service Stations"},
	{code:"912103", title:"Government Offices-County"},
	{code:"912102", title:"Government Offices-State"},
	{code:"912101", title:"Government Offices-US"},
	{code:"541105", title:"Grocers-Retail"},
	{code:"152105", title:"Home Improvements"},
	{code:"806202", title:"Hospitals"},
	{code:"701101", title:"Hotels & Motels"},
	{code:"641112", title:"Insurance"},
	{code:"823106", title:"Libraries"},
	{code:"799951", title:"Parks"},
	{code:"171105", title:"Plumbing Contractors"},
	{code:"922104", title:"Police Departments"},
	{code:"431101", title:"Post Offices"},
	{code:"573501", title:"Records Tapes & Compact Discs-Retail"},
	{code:"735910", title:"Rental Service-Stores & Yards"},
	{code:"581208", title:"Restaurants"},
	{code:"594201", title:"Retail-Book Dealers"},
	{code:"599201", title:"Retail-Florists"},
	{code:"541105", title:"Retail-Grocers"},
	{code:"525104", title:"Retail-Hardware"},
	{code:"594113", title:"Retail-Sporting Goods"},
	{code:"651201", title:"Shopping Centers & Malls"},
	{code:"412101", title:"Taxicabs & Transportation Service"},
	{code:"783201", title:"Theatres"}
];

function $(id) {
	return document.getElementById(id);
}

function getLocalizedString(string) {
	try { string = localizedStrings[string] || string; } catch (e) {}
	return string;
}

function createKey(key) {
	if (window.widget) {
        key = widget.identifier + "-" + key;
	}
	return key;
}

function setInstanceAndGlobalPreferenceForKey(value, key) {
	setInstancePreferenceForKey(value, key);
	setGlobalPreferenceForKey(value, key)
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
		result = gDefaultPreferencesTable[key];
	}
	return result;
}

function intPreferenceForKey(key) {
	return parseInt(preferenceForKey(key), 10);
}

function getComputedPropertyValue(el, property, pseudoClass) {
	return document.defaultView.getComputedStyle(el, pseudoClass)[property];
}

function load() {
	localizeStrings();
	setupButtons();
	setupProgressIndicator();
	handleVirginLaunch();
	loadPreferences();
	loadXslt();
	preloadImages();
	addEventListenters();
	selectAllInSearchTextField();
}

function onsync() {
	loadPreferences();
	clearUI();
	shrinkWidget();
}

function onshow() {
	if (gBusy) {
		startProgressAnimation();
	}
}

function onhide() {
	stopProgressAnimation();
	hideSearchPopUp();
	YellowPages.hideLargeType();
}

function onremove() {
	setInstancePreferenceForKey(null, kLocationKey);
	setInstancePreferenceForKey(null, kSearchRadiusKey);
	setInstancePreferenceForKey(null, kItemsPerPageKey);
	setInstancePreferenceForKey(null, kWindowHeightKey);
}

function blur() {
	hideSearchPopUp();
}

function localizeStrings() {
	setInnerText("cityStateLabel", "City, State");
	setInnerText("orPostalCodeLabel", "or ZIP Code:");
	setInnerText("searchRadiusLabel", "Search within:");
	setInnerText("itemsPerPageLabel", "Items per page:");
	setInnerText("addConfirmCancelButton", "Cancel");
	setInnerText("addConfirmAddButton", "Add");
	setInnerText("addConfirmMessage", "Add this entry to Address Book?");
	setInnerText("prevButton", "Previous");
	setInnerText("nextButton", "Next");
	setInnerText("noResultsWrap", "NoResults");

	$("logo").setAttribute("alt", getLocalizedString("Visit Directory Assistance Plus website"));
	$("searchTextField").setAttribute("placeholder", getLocalizedString("Business Name or Category"));
	document.title = getLocalizedString("Phone Book");
}

function setupButtons() {
	new AppleInfoButton($("infoButton"), $("front"), "black", "black", doShowBackResizeAnimation);
	new AppleGlassButton($("doneButton"), getLocalizedString("Done"), showFront);
}

function setupResultsListScrollArea() {
	if (!gResultsListScrollArea) {
		var sb = new AppleVerticalScrollbar($("resultsListScroller"));
		sb.setTrackStart("Images/trannie.png", 10);
		sb.setTrackMiddle("Images/trannie.png");
		sb.setTrackEnd("Images/trannie.png", 10);
		gResultsListScrollArea = new AppleScrollArea($("resultsListWrap"), sb);
	}
}

function refreshResultsListScrollArea() {
	if (gResultsListScrollArea) {
		gResultsListScrollArea.refresh();
	}
}

function setupProgressIndicator() {
	gProgressIndicator = new AppleProgressIndicator($("progressIndicatorCanvas"));
	gProgressIndicator.setRGB(0, 0, 0);
}

function setupCategoryBrowser() {
	var el = $("categoryBrowser");
	var delegate = new MyBrowserDelegate();
	var colWidth = parseInt(getComputedPropertyValue(el, "width"));
	gBrowser = new AppleBrowser(el, delegate, colWidth);
}

function setInnerText(id, string) {
	$(id).innerText = getLocalizedString(string);
}

function selectAllInSearchTextField() {
	var searchTextField = $("searchTextField");
	searchTextField.focus();
	searchTextField.select();
}

function preloadImages() {
	var img = new Image();
	img.src = "Images/bodycompact_expanded.png";
	img.src = "Images/add_onmouseover.png";
	img.src = "Images/add_onclick.png";
	img.src = "Images/popupbutton_clicked.png";
	img.src = "Images/confirmbackground.png";
	img.src = "Images/confirmbutton.png";
	img.src = "Images/backarrow_on.png";
	img.src = "Images/forwardarrow_on.png";
	img.src = "Images/backarrow_clicked.png";
	img.src = "Images/forwardarrow_clicked.png";
	img.src = "Images/Activity_StopPressed.tif";
	img.src = "Images/scrollBar.png";
}

function addEventListenters() {
	document.addEventListener("mousedown", documentMouseDown, false);
	document.addEventListener("keydown", documentKeyDown, false);

	$("resizer").addEventListener("mousedown", resizerMouseDown, false);
	
	var searchTextField = $("searchTextField");
	searchTextField.addEventListener("search", searchTextFieldSearched, false);
	searchTextField.addEventListener("keydown", searchTextFieldKeyDown, false);
	searchTextField.addEventListener("keyup", searchTextFieldKeyUp, false);
	
	$("locationTextField").addEventListener("keydown", locationTextFieldKeyDown, false);
	
	if (window.widget) {
		widget.onsync = onsync;
		widget.onshow = onshow;
		widget.onhide = onhide;
		widget.onremove = onremove;
	}
	window.onblur = blur;	
}

function resizerMouseDown(evt) {
	var resizer = $("resizer");
	document.addEventListener("mousemove", resizerMouseMove, false);
	document.addEventListener("mouseup", resizerMouseUp, false);
	
	var frontHeight = parseInt(getComputedPropertyValue($("front"), "height"));
	resizerMouseMove.delta = frontHeight - evt.clientY;
	
}

function resizerMouseMove(evt) {
	var y = evt.pageY + resizerMouseMove.delta;
	y = (y < kMinExpandedWindowHeight) ? kMinExpandedWindowHeight : y;
	y = (y > gCurrentMaxExpandedWindowHeight) ? gCurrentMaxExpandedWindowHeight : y;
	
	if (window.widget) {
		widget.resizeAndMoveTo(window.screenX, window.screenY, window.innerWidth, y);
	}
	
	$("front").style.height = y + "px";
	refreshResultsListScrollArea();
	consumeEvent(evt);
}

function resizerMouseUp(evt) {
	document.removeEventListener("mousemove", resizerMouseMove, false);
	document.removeEventListener("mouseup", resizerMouseUp, false);
	setInstancePreferenceForKey(window.innerHeight, kWindowHeightKey);
	refreshResultsListScrollArea();
	consumeEvent(evt);
}

function documentMouseDown(evt) {
	hideSearchPopUp();
	if (gCurrentState == kStateCollapsed) {
		selectAllInSearchTextField();
	} else if (isNodeResultListItem(evt.target)) {
		focusResultsListScrollArea();
	}
}

function isNodeResultListItem(node) {
	return node.nodeName.toLowerCase() == "li";
}

function documentKeyDown(evt) {
	if (AppleBrowser._isEscKeyEvent(evt)) {
		hideSearchPopUp();
		$("searchTextField").value = "";
		selectAllInSearchTextField();
		searchTextFieldKeyDown.valueChanged = true;
		consumeEvent(evt);
	} else if (13 == evt.keyCode) { // return or enter
		if (ElementUtils.isShowing($("searchPopUp"))) {
			activateSelectedBrowserCell();
			consumeEvent(evt);
		}
	} else if (70 == evt.keyCode && evt.metaKey) { // cmd-f
		hideSearchPopUp();
		selectAllInSearchTextField();
		consumeEvent(evt);
	}
	else if (AppleBrowser._isLeftArrowKeyEvent(evt) && $("prevButton").disabled != true) {
		prevButtonClicked(evt);
	}
	else if (AppleBrowser._isRightArrowKeyEvent(evt) && $("nextButton").disabled != true) {
		nextButtonClicked(evt);
	}	
}

function searchPopUpCellClicked(evt) {
	activateSelectedBrowserCell();
	ElementUtils.show($("clearSearchTextFieldButton"));
	consumeEvent(evt);
}

function activateSelectedBrowserCell(categoryCode) {
	hideSearchPopUp();
	selectAllInSearchTextField();

	var cell = gBrowser.selectedCellInColumn(0);
	var categoryCode = cell.id.substring(1);
	createSearchCommand(categoryCode);
	search();
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

ElementUtils.isShowing = function(el) {
	return el.style.display != "none";
};

var WidgetUtils = {};

WidgetUtils.resizeTo = function(w, h) {
	w = (-1 == w) ? window.innerWidth  : w;
	h = (-1 == h) ? window.innerHeight : h;
	ElementUtils.resizeTo($("front"), w, h);
	if (window.widget) {
		window.resizeTo(parseInt(w, 10), parseInt(h, 10));
	}
};

function consumeEvent(evt) {
	if (evt) {
		try {
			evt.stopPropagation();
			evt.preventDefault();
		} catch (e) {}
	}
}

function doShowBackResizeAnimation(evt) {
	var expanding = false;
	doResizeAnimation(expanding, showBack, kMinExpandedWindowRect);
}

function doShowFrontResizeAnimation() {
	var expanding = true;
	doResizeAnimation(true, doShowFrontResizeAnimationComplete, kMinExpandedWindowRect);
}

function doShowFrontResizeAnimationComplete() {
	if (gShouldSearchWhenFrontShown) {
		search();
	}
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
	setTimeout(backShown, 700);
}

function showFront() {
	savePreferences();
	
	if (window.widget) {
		widget.prepareForTransition("ToFront");
	}
	var front = $("front");
	ElementUtils.hide($("back"));
	ElementUtils.show(front);
	front.className = "resizing";
	if (window.widget) {
		setTimeout("widget.performTransition();", 0);
	}

	setTimeout(doShowFrontResizeAnimation, 700);
}

function backShown() {
	var locationTextField = $("locationTextField");
	locationTextField.focus();
	locationTextField.select();
}

function handleVirginLaunch() {
	if (globalLocationPreferenceIsSet()) {
		return;
	}
	setVirginLaunchLocation();
	// preference migration
	setInstanceAndGlobalPreferenceForKey(null, "category");
	setInstanceAndGlobalPreferenceForKey(null, "itemsperpage");
	setInstanceAndGlobalPreferenceForKey(null, "lastSearchTerm");
	setInstanceAndGlobalPreferenceForKey(null, "searchwithin");	
}

function globalLocationPreferenceIsSet() {
	if (window.widget) {
		return null != widget.preferenceForKey(kLocationKey);
	}
	// if run outside of Dashboard env, 
	// you can's set prefs anyway, so just return true
	return true;
}

function setVirginLaunchLocation() {
	if (window.YellowPages) {
		// addrArray == ["Sunnyvale","CA","94087", undefined]
		var addrArray = YellowPages.YPgetUserAddress();
		var location;
		if (addrArray && addrArray.length > 2) {
			location = addrArray[2];
		} else {
			location = kDefaultLocation;
		}
		setInstanceAndGlobalPreferenceForKey(location, kLocationKey);
	}
}

function loadPreferences() {
	var location = preferenceForKey(kLocationKey);
	$("locationTextField").value = location;
	setInstanceAndGlobalPreferenceForKey(location, kLocationKey);

	var searchRadius = preferenceForKey(kSearchRadiusKey);
	$("searchRadiusPopUp").value = searchRadius;
	setInstanceAndGlobalPreferenceForKey(searchRadius, kSearchRadiusKey);

	var itemsPerPage = preferenceForKey(kItemsPerPageKey);
	$("itemsPerPagePopUp").value = itemsPerPage;
	setInstanceAndGlobalPreferenceForKey(itemsPerPage, kItemsPerPageKey);
}

function savePreferences() {
	var location = $("locationTextField").value;
	setInstanceAndGlobalPreferenceForKey(location, kLocationKey);
	
	var searchRadius = parseInt($("searchRadiusPopUp").value, 10);
	setInstanceAndGlobalPreferenceForKey(searchRadius, kSearchRadiusKey);

	var itemsPerPage = parseInt($("itemsPerPagePopUp").value, 10);
	setInstanceAndGlobalPreferenceForKey(itemsPerPage, kItemsPerPageKey);
	
	if (!gSearchCommand) {
		createSearchCommand();
	}
	gSearchCommand.location = location;
	gSearchCommand.searchRadius = searchRadius;
	gSearchCommand.itemsPerPage = itemsPerPage;
}

function loadXslt() {
	sendAsyncGetRequest("PhoneBook.xsl", xsltLoaded);
}

function xsltLoaded(evt, req) {
	gXstlDocument = req.responseXML;
	evt = req = null;
}

function doResizeAnimation(expanding, callback, altSmallRect) {
	var front = $("front");
	if (expanding) {
		if ("expanded" == front.className) {
			return;
		}
		ElementUtils.show($("middlePane"));
		ElementUtils.show($("bottomPane"));
	} else {
		if ("expanded" != front.className) {
			return;
		}
		ElementUtils.hide($("resultsListWrap"));
		refreshResultsListScrollArea();
	}
	gCurrentState = kStateResizing;
	front.className = "resizing";

	var animator = new AppleAnimator(kResizeAnimationDuration, kResizeAnimationInterval);
	
	var smallRect = new AppleRect(0, 0, kDefaultWindowWidth, kCollapsedWindowHeight);
	var largeRect = new AppleRect(0, 0, kDefaultWindowWidth, intPreferenceForKey(kWindowHeightKey));

	var animation = new AppleRectAnimation(
		(expanding ? (altSmallRect ? altSmallRect : smallRect) : largeRect),
		(expanding ? largeRect : (altSmallRect ? altSmallRect : smallRect)),
		resizeAnimationHandler);
		
	animator.addAnimation(animation);
	animator.oncomplete = function() {resizingAnimationCompleteHandler(expanding, callback);};
	animator.start();
}

function resizeAnimationHandler(animator, currRect, startRect, finRect) {
	if(currRect.bottom > 66)
		$("bottomPaneMask").className = $("topPane").className = "bottomPanePastBottomOfTopPane";
	else
		$("bottomPaneMask").className = $("topPane").className = "";

	WidgetUtils.resizeTo(currRect.right, currRect.bottom);
}

function resizingAnimationCompleteHandler(expanding, callback) {
	if (expanding) {
		$("front").className = "expanded";
		gCurrentState = kStateExpanded;
		ElementUtils.show($("resultsListWrap"));
		setupResultsListScrollArea();
		refreshResultsListScrollArea();
		focusResultsListScrollArea();
	} else {
		$("front").className = "collapsed";
		gCurrentState = kStateCollapsed;
	}
	if (callback) {
		callback();
	}
}

function expandWidget() {
	doResizeAnimation(true);
}

function shrinkWidget() {
	doResizeAnimation(false);
}

function searchTextFieldKeyDown(evt) {
	var searchTextField = $("searchTextField");
	blurResultsListScrollArea();
	if (AppleBrowser._isDownArrowKeyEvent(evt)) {
		searchTextField.blur();
		if (searchTextFieldKeyDown.valueChanged) {
			searchTextFieldKeyDown.valueChanged = false;
			if (gBrowser) {
				gBrowser.selectRowInColumn(0, 0);
			}
		}
		showSearchPopUp();
		consumeEvent(evt);
	} else if (AppleBrowser._isEscKeyEvent(evt) && $("searchTextField").value == "") {
		clearSearchTextFieldButtonClicked(event);
		consumeEvent(evt);
	} else if (AppleBrowser._isAlphaKeyEvent(evt)) {
		searchTextFieldKeyDown.valueChanged = true;
	}
}

function searchTextFieldKeyUp(evt) {
	if (gCurrentState == kStateExpanded) {
		ElementUtils.show($("clearSearchTextFieldButton"));
		return;
	}
	var str = searchTextField.value;
	if (str && str.length) {
		ElementUtils.show($("clearSearchTextFieldButton"));
	} else {
		ElementUtils.hide($("clearSearchTextFieldButton"));
	}
}

function SearchCommand(searchString, location, searchRadius, startIndex, itemsPerPage, isKeyword) {
	this.searchString = searchString;
	this.location	  = location;
	this.searchRadius = searchRadius;
	this.startIndex   = startIndex;
	this.itemsPerPage = itemsPerPage;
	this.isKeyword    = isKeyword;
}

function searchTextFieldSearched(evt) {
	var str = $("searchTextField").value;
	if (!str || !str.length) {
		return;
	}
	str = str.toLowerCase();

	// check to see if search string matches a category title.
	// if so, just use the category code
	var categoryCode = categoryCodeForSearchString(str);
	createSearchCommand(categoryCode);
	search();
}

function categoryCodeForSearchString(str) {
	for (var i=0; i < gCategories.length; i++) {
		var cat = gCategories[i];
		if (cat.title.toLowerCase() == str) {
			return cat.code;
		}
	}
	return null;
}

function createSearchCommand(categoryCode) {
	var isKeyword = (categoryCode != null);
	var searchString;
	
	if (isKeyword) {
		searchString = categoryCode;
	} else {
		searchString = $("searchTextField").value;
	}

	var location = preferenceForKey(kLocationKey);
	var searchRadius = intPreferenceForKey(kSearchRadiusKey);
	var itemsPerPage = intPreferenceForKey(kItemsPerPageKey);
	var startIndex = 0;
	
	gSearchCommand = new SearchCommand(searchString, 
									   location, 
									   searchRadius, 
									   startIndex,
									   itemsPerPage, 
									   isKeyword);
}

function search() {
	clearUI();
	expandWidget();
	setBusy(true);
	sendSearchRequest(gSearchCommand);	
	selectAllInSearchTextField();
}

function clearUI() {
	$("navText").innerText = "";
	$("prevButton").setAttribute("disabled", "disabled");
	$("nextButton").setAttribute("disabled", "disabled");
	$("resultsListWrap").innerHTML = "";
	ElementUtils.hide($("noResultsWrap"));
	refreshResultsListScrollArea();
}

function setBusy(busy) {
	gBusy = busy;
	if (gBusy) {
		startProgressAnimation();
	} else {
		stopProgressAnimation();
	}
}

function startProgressAnimation() {
	if (gBusy) {
		gProgressIndicator.startAnimation();
	}
}

function stopProgressAnimation() {
	gProgressIndicator.stopAnimation();
}

function handleSearchResults(doc) {
	setBusy(false);
	stopProgressAnimation();

	if (!doc || 0 == gTotalResults) {
		ElementUtils.show($("noResultsWrap"));
		return;
	}
	
	var proc = new XSLTProcessor();
	proc.importStylesheet(gXstlDocument);
	var res = proc.transformToFragment(doc, document);
	
	$("resultsListWrap").appendChild(res);
	refreshResultsListScrollArea();
	$("searchTextField").blur();
	focusResultsListScrollArea();
	updateNavigation();
	
	// cleanup just in case
	proc.reset();
	res = evt = doc = req = proc = null;
}

function focusResultsListScrollArea() {
	if (gResultsListScrollArea && !ElementUtils.isShowing($("searchPopUp"))) {
		gResultsListScrollArea.focus();
	}
}

function blurResultsListScrollArea() {
	if (gResultsListScrollArea) {
		gResultsListScrollArea.blur();
	}
}

function updateNavigation() {
	var startIndex = gSearchCommand.startIndex;
	var endIndex = startIndex + gSearchCommand.itemsPerPage;
	endIndex = (endIndex > gTotalResults) ? gTotalResults : endIndex;
	
	var prevButtonDisabled = (0 == startIndex);
	var nextButtonDisabled = (endIndex >= gTotalResults);
	
	if (prevButtonDisabled) {
		$("prevButton").setAttribute("disabled", "disabled");
	} else {
		$("prevButton").removeAttribute("disabled");
	}
	
	if (nextButtonDisabled) {
		$("nextButton").setAttribute("disabled", "disabled");
	} else {
		$("nextButton").removeAttribute("disabled");
	}

	var navTextStr = (startIndex+1) + "-" + endIndex;
	if (gTotalResults > gSearchCommand.itemsPerPage) {
		navTextStr += " " + getLocalizedString("of") + " " + gTotalResults;
	}
	$("navText").innerText = navTextStr;

}

function searchPopUpButtonMouseDown(evt) {
	if (gCurrentState == kStateResizing) {
		return;
	}
	var searchPopUp = $("searchPopUp");
	if (ElementUtils.isShowing(searchPopUp)) {
		hideSearchPopUp();
	} else {
		showSearchPopUp();
	}
	consumeEvent(evt);
}

function hideSearchPopUp() {
	if (ElementUtils.isShowing(searchPopUp)) {
		gBrowser.blur();
		ElementUtils.hide(searchPopUp);
		if (gCurrentState == kStateCollapsed) {
			WidgetUtils.resizeTo(kDefaultWindowWidth, kCollapsedWindowHeight);
		}
	}
}

function showSearchPopUp() {
	if (gCurrentState == kStateCollapsed) {
		WidgetUtils.resizeTo(kDefaultWindowWidth, kMinExpandedWindowHeight);
	}
	blurResultsListScrollArea();
	ElementUtils.show(searchPopUp);
	if (!gBrowser) {
		setupCategoryBrowser();
		gBrowser.selectRowInColumn(0, 0);
	} else if (searchTextFieldKeyDown.valueChanged) {
		gBrowser.selectRowInColumn(0, 0);
	}
	var selectedIndex = gBrowser.path()[0];
	gBrowser.selectRowInColumn(selectedIndex, 0);
	gBrowser.focus();
}

function logoClicked(evt) {
	if (window.widget) {
		widget.openURL(getVendorHomepageUrl());
	}
}

function addButtonClicked(evt, bizId) {
	gCurrentBizId = bizId;
	var bizNameEl = $("name-" + bizId);
	setInnerText("addConfirmName", bizNameEl.textContent);
	ElementUtils.show($("addConfirmDialog"));
	$("addConfirmCancelButton").focus();
}

function addConfirmCancelButtonClicked(evt) {
	ElementUtils.hide($("addConfirmDialog"));
}

function getCurrentAddressData() {
	var name 		= $("name-"			+gCurrentBizId).textContent;
	var street 		= $("street-"		+gCurrentBizId).textContent;
	var city 		= $("city-"			+gCurrentBizId).textContent;
	var state 		= $("state-"		+gCurrentBizId).textContent;
	var postalCode 	= $("postalCode-"	+gCurrentBizId).textContent;
	var phone 		= $("phone-"		+gCurrentBizId).textContent;
	return {name:name, street:street, city:city, 
		state:state, postalCode:postalCode, phone:phone};
}

function addConfirmAddButtonClicked(evt) {
	ElementUtils.hide($("addConfirmDialog"));
	var addr = getCurrentAddressData();
	YellowPages.YPaddToAddressBook(addr.name, addr.street, addr.city, 
		addr.state, addr.postalCode, addr.phone);
}

function prevButtonClicked(evt) {
	gSearchCommand.startIndex -= gSearchCommand.itemsPerPage;
	gSearchCommand.startIndex = (gSearchCommand.startIndex < 0) ? 0 : gSearchCommand.startIndex;
	search();
}

function nextButtonClicked(evt) {
	gSearchCommand.startIndex += gSearchCommand.itemsPerPage;
	gSearchCommand.startIndex = (gSearchCommand.startIndex > gTotalResults) ? gTotalResults : gSearchCommand.startIndex;
	search();
}

function clearSearchTextFieldButtonClicked(evt) {
	shrinkWidget();
	ElementUtils.hide($("clearSearchTextFieldButton"));
	$("searchTextField").value = "";
	selectAllInSearchTextField();
}

function phoneClicked(evt) {
	YellowPages.showLargeType(evt.target.textContent);
}

function bizNameClicked(evt, bizId) {
	if (window.widget) {
		widget.openURL(getBizProfileUrl(bizId));
	}
}

function addressClicked(evt, bizId) {
	if (window.widget) {
		widget.openURL(getBizMapUrl(bizId));
	}
}

function locationTextFieldKeyDown(evt) {
	settingsOnBackChanged();
	if (13 == evt.keyCode) { // enter or return
		var str = $("locationTextField").value;
		if (!str || !str.length) {
			//TODO do something?
			return;
		}
		str = str.replace("-", "");
		if (isFinite(str)) { // it's a postalCode, don't validate
			return;
		}
		sendValidationRequest(str);
		consumeEvent(evt);
	}
}

function searchRadiusPopUpChanged(evt) {
	settingsOnBackChanged();
}

function itemsPerPagePopUpChanged(evt) {
	settingsOnBackChanged();
}

function settingsOnBackChanged() {
	gShouldSearchWhenFrontShown = true;
	gSearchCommand.startIndex = 0;
}

function showLocationValidationResults(names) {
	if (!window.widget) {
		return;
	}
	var menu = widget.createMenu();
	var x = 31, y = 82;
	
	if (!names) {
		menu.addMenuItem(getLocalizedString("Specify City, State or ZIP Code:"));
		menu.setMenuItemEnabledAtIndex(0, false);
		menu.popup(x, y);
	} else if (names && 0 == names.length) {
		menu.addMenuItem(getLocalizedString("No cities found"));
		menu.setMenuItemEnabledAtIndex(0, false);
		menu.popup(x, y);
	} else if (1 == names.length) {
		$("locationTextField").value = names[0];
	} else {
		for (var i=0; i < names.length; i++) {
			menu.addMenuItem(names[i]);
		}
		var selectedIndex = menu.popup(x, y);
		if (selectedIndex > -1) {
			$("locationTextField").value = names[selectedIndex];
		}
	}
}

function MyBrowserDelegate() {}

MyBrowserDelegate.prototype.numberOfRowsInColumn = function(browser, colIndex) {
	return colIndex ? 0 : gCategories.length;
};

MyBrowserDelegate.prototype.selectRowInColumn = function(browser, rowIndex, colIndex) {
	var cell = gBrowser.selectedCellInColumn(colIndex);
	var str = cell.innerText;
	$("searchTextField").value = str;
};

MyBrowserDelegate.prototype.willDisplayCellAtRowColumn = function(browser, cell, rowIndex, colIndex) {
	var cat = gCategories[rowIndex];
	cell.innerText = cat.title;
	cell.id = "_" + cat.code;
	cell.addEventListener("click", searchPopUpCellClicked, false);
};
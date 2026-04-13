//
//  AddressBook.js
//  Widgets
/*
Copyright 2005, Apple Computer, Inc.  All rights reserved.
NOTE:  Use of this source code is subject to the terms of the Software
License Agreement for Mac OS X, which accompanies the code.  Your use
of this source code signifies your agreement to such license terms and
conditions.  Except as expressly granted in the Software License Agreement
for Mac OS X, no other copyright, patent, or other intellectual property
license or right is granted, either expressly or by implication, by Apple.
*/

/*======================================================================
 *	Globals and Global Events
 *======================================================================*/


var gABCurrentCardIndex = 0;
var gSavedSearchString = "";
var gSkipNextCompletion = false;
var gDisplayLog = 0;

var kMinimumHeight = 87;
var kMinimumWidth =  288;
var gCurrentWidth = 288;
var kMinimumExpandedHeight = 145;
var kDefaultExpandedHeight = 400;

var AddressBookPlugin;

function $(id) {
	return document.getElementById(id);
}

function consumeEvent(evt) {
	evt.stopPropagation();
	evt.preventDefault();
}

function setup() {
	$("pageBottomContent").style.visibility = 'hidden';
	var searchField = $('SearchInputElement');
	searchField.setAttribute("placeholder", getLocalizedString('Name'));
	searchField.focus();
	View.setCurrent(TitleView);
}

if (window.widget) {
	widget.onshow = onShow;
	widget.onhide = onHide;
//	widget.onremove = function () {Preference.removeInstancePreferences();};
}

window.onfocus = function () {
	if (Window.expanded) {
		Scroller.show();
		$("pageBottomContent").style.visibility = 'visible';
		Arrows.show();
	} else {
		Scroller.hide();
		$("pageBottomContent").style.visibility = 'hidden';
	}
	Window.bottomImage.src = 'Images/Bottom_'+(Window.expanded?'Dark':'Light')+'.png';
	
	// bring back the snapback button
	var element = $('SearchInputElementSnapbackIcon');
	if (element != null)
		element.src = 'Images/Search_SnapBack.png';
}

window.onblur = function () {
	var searchElement = $('SearchInputElement');
	if ( searchElement.value == "")
		shrink();
	
	Scroller.blur();
	Scroller.hide();
	
	$("pageBottomContent").style.visibility = 'hidden';
	Window.bottomImage.src = 'Images/Bottom_Light.png';
	document.addEventListener("mousedown", eatMouse, false);
	Arrows.hide();

	// hid the snapback button
	var element = $('SearchInputElementSnapbackIcon');
	if (element != null)
		element.src = 'url("Images/Search_SnapBackCover.png")';
}

function eatMouse(evt) {
	consumeEvent(evt);
	document.removeEventListener("mousedown", eatMouse, false);
	$('SearchInputElement').focus();
}

/*======================================================================
 *	Window
 *======================================================================*/
// Construct the Address Book widget window	
var Window = function () {
	// Address Book window is created with the shrunk state (defined in Info.plist)
	Window.expanded = false;
	Window.shrunkHeight = window.innerHeight;

	Window.page = $("pageScroll");
	Window.pageStyle = Window.page.style;
	Window.bottomStyle = $("bottombar").style;
	Window.bottomImage = $("bottombarImage");

	Window.initialSize = new Position(kMinimumWidth,kMinimumHeight);
	
	var y = Preference.get("windowHeight");
	Window.expandedHeight = y ? y: kDefaultExpandedHeight;
}
window.addEventListener('load', Window, false);

Window.currentSize = function () {
	return new Position(gCurrentWidth, window.innerHeight);
}

Window.resizeTo = function ( newSize, status) {
	// limit the size
	if ( status == "tracking" || status == "trackingEnd")	
		newSize.x = gCurrentWidth;

	if ( ! Window.expanded || (status == "expanding") ) {
		if ( newSize.y < kMinimumHeight)	newSize.y = kMinimumHeight;
	} else {
		if ( newSize.y < kMinimumExpandedHeight)	newSize.y = kMinimumExpandedHeight;
	}

	var currentSize = Window.currentSize();
	if ( ! newSize.equal(currentSize) || status != "tracking" ) {

		window.resizeTo(newSize.x,newSize.y);

		Window.pageStyle.height = (newSize.y-27) + "px";
		$("Middle").style.height = (newSize.y-58) + "px";
		Window.bottomStyle.top = (Window.pageStyle.height - 31) + "px";
		Scroller.refresh();

		// set the window size pref at the end
		if ( status != "trackingEnd" && Window.expanded) {
			Window.initialSize.set(newSize);
			Preference.set("windowHeight", newSize.y);
			Window.expandedHeight = newSize.y;
		}
	}
}

Window.hide = function () {
	// noop
}


/*
 *  Events
 */

function onShow()
{
	$('SearchInputElement').focus();
}

function onHide()
{
	AddressBookPlugin.hideLargeType();
}


/*======================================================================
 *	ExpandShrink Animation
 *======================================================================*/

function expand (func) { ExpandShrink.animate(1,func);}
function shrink (func) { ExpandShrink.animate(0,func);}

var ExpandShrink = new Object ();

ExpandShrink.animator = function () {
	if ( ! ExpandShrink._animator) {
		var duration = 400;
		ExpandShrink._animator = new AppleAnimator (duration, 13);
		ExpandShrink._animator.oncomplete = ExpandShrink.onFinish;
	}
	return ExpandShrink._animator;
}

ExpandShrink.adjust = function (animator, now, first, done) {
	Window.resizeTo(new Position(kMinimumWidth, Math.round(now)), "expanding");
}

ExpandShrink.onFinish = function () {
	if (ExpandShrink._animation) {
		delete ExpandShrink._animation;
		ExpandShrink._animation = null;
	}
	if (ExpandShrink._animator) {
		delete ExpandShrink._animator;
		ExpandShrink._animator = null;
	}

	if (Window.expanded) {
		Scroller.show();
		View.focus();
		$("pageBottomContent").style.visibility = 'visible';
		Arrows.show();
	} else {
		View.setCurrent(TitleView);
//		Window.bottomStyle.background = 'url(Images/Bottom_Light.png) no-repeat';
	}
	if (ExpandShrink.callback)	setTimeout(ExpandShrink.callback,0);
}

ExpandShrink.animate = function (expand,callback) {
	if (Window.expanded == expand) return;
	Window.expanded = expand;	// todo:is this the best way ?

	ExpandShrink.callback = callback;
	var animator = ExpandShrink.animator();
	animator.oncomplete = ExpandShrink.onFinish;
	
	var from = expand ? kMinimumHeight: Window.currentSize().y;
	var to = expand ? Math.max(Window.expandedHeight, kMinimumExpandedHeight) : kMinimumHeight;
	
	ExpandShrink._animation = new AppleAnimation(from, to, ExpandShrink.adjust);
	animator.addAnimation(ExpandShrink._animation);

	if (expand) {
		View.setCurrent(NameListView);
		Window.bottomImage.src = 'Images/Bottom_Dark.png';
	} else {
		Scroller.hide();
		$("pageBottomContent").style.visibility = 'hidden';
		setCurrentPhotoToGeneric();
		Arrows.hide();

		Window.bottomImage.src = 'Images/Bottom_Light.png';
	}

	animator.start();
}

/*======================================================================
 *	Search Field
 *======================================================================*/

var SearchField = function () {
}

SearchField.onSearch = function (searchField) {
	var newSearch = searchField.value;
	var oldSearch = gSavedSearchString;

	var sameSearchAgain = newSearch != oldSearch;
	SearchField.performSearchAndSelectBestMatch(searchField.value, sameSearchAgain);
}

SearchField.performSearchAndSelectBestMatch = function(searchString, selectBestMatch)
{
	gSavedSearchString = searchString;
	
	if (selectBestMatch)
		gABCurrentCardIndex = AddressBookPlugin.searchForStringWithBestMatch(gSavedSearchString);
	else
		gABCurrentCardIndex = AddressBookPlugin.searchForStringWithoutBestMatch(gSavedSearchString);
	
	AddressBookPlugin.displayCardAtIndex(gABCurrentCardIndex);
	View.newSearch();
}


/*======================================================================
 *	Scroller
 *======================================================================*/
var Scroller = function () {
	var element = $("scrollbar");
	Scroller.scroller = new AppleVerticalScrollbar(element);
	Scroller.scrollerStyle = element.style;
	Scroller.scrollerStyle.visibility = "hidden";
}
window.addEventListener('load', Scroller, false);

Scroller.focus = function () {
	if (Scroller.view)
		Scroller.view.focus();
}

Scroller.blur = function () {
	if (Scroller.view)
		Scroller.view.blur();
}

Scroller.hide = function () {
	Scroller.scrollerStyle.visibility = "hidden";
}

Scroller.show = function () {
	if (Window.expanded)
		Scroller.scrollerStyle.visibility = "visible";
}

Scroller.setView = function (view) {
	if (Scroller.view) {
		Scroller.view.removeScrollbar(Scroller.scroller);
		delete Scroller.view;
	}
	Scroller.view = new AppleScrollArea(view);
	Scroller.view.addScrollbar(Scroller.scroller);
}

Scroller.refresh = function () {
	if (Scroller.view)
		Scroller.view.refresh();
}

/*======================================================================
 *	Resize Box
 *======================================================================*/

var ResizeBox = new Object ();

ResizeBox.mouseDown = function (evt) {
	ResizeBox.clickOffset = Window.currentSize().sub(evt);
	document.addEventListener("mousemove", ResizeBox.mouseMove, false); 
	document.addEventListener("mouseup", ResizeBox.mouseUp, false); 
	TrackMouse.tracking = true;
}
ResizeBox.mouseMove = function (evt) {
	Window.resizeTo(ResizeBox.clickOffset.add(evt),"tracking"); 
}
ResizeBox.mouseUp = function (evt) { 
	TrackMouse.tracking = false;
	Window.resizeTo(ResizeBox.clickOffset.add(evt), "trackingEnd"); 
    document.removeEventListener("mousemove", ResizeBox.mouseMove, false);
    document.removeEventListener("mouseup", ResizeBox.mouseUp, false);
}
ResizeBox.hide = function () {
	var element = $("ResizeBox");
	element.style.visibility = "hidden";
}
ResizeBox.show = function () {
	var element = $("ResizeBox");
	element.style.visibility = "visible";
}

/*======================================================================
 *	View management
 *======================================================================*/
var View = new Object ();

View.focus = function () {
}

View.setCurrent = function (newView) {
	if ( View._current != newView ) {
		if ( View._current)	View._current.hide();
		newView.activate();
		newView.style.visibility = "visible";
		View._current = newView;
	}
	setTimeout(View.focus,0);
}

View.getCurrent = function () {
	if ( typeof View._current == 'undefined')	View._current = null;
	return View._current;
}

View.isCurrentViewNameListView = function () {
	return View.getCurrent() == NameListView;
}

View.isCurrentViewTitleView = function () {
	return View.getCurrent() == TitleView;
}

View.isCurrentViewDetailView = function () {
	return View.getCurrent() == CardDetailView;
}

View.newSearch = function () {
	View._string = $('SearchInputElement').value;

	if ( View._string.length == 0 ) {
		if ( Window.expanded)
			View.clear();
	} else {
		if ( Window.expanded) {
			View._newSearch();
		} else {
			expand(View._newSearch);
		}
	}
}

View._newSearch = function ()
{
	var count = AddressBookPlugin.count();
	var tCount = AddressBookPlugin.totalCount();
	if ( (View.getCurrent().viewKind != NameListView.viewKind) ) { // && (count != tCount) ) {
		
		View._prepareForSearch();
	
		View.setCurrent(NameListView);
	}
	View.getCurrent().refresh();
}

View._prepareForSearch = function () {
	gSkipNextCompletion = false;
	NameListView.resetForNewSearch();
	CardDetailView.resetForNewSearch();
}

View.clear = function () {
	View._prepareForSearch();

	if (Window.expanded)	shrink();
}

/*======================================================================
 *	Name List View
 *======================================================================*/
NameListView = function () {
	var element = $('NameListViewBlock');

	NameListView.element = element;
	NameListView.style = element.style;
}
window.addEventListener('load', NameListView, false);

NameListView.resetForNewSearch = function () {
	NameListView.clear();
}

NameListView.viewKind = "NameList";

NameListView.activate = function () {
	if (View.getCurrent() != NameListView.viewKind) {
		NameListView.active = 0;
		document.addEventListener('keydown', NameListView.keyDown, false);
		document.addEventListener('keypress', NameListView.keyPress, false);
		document.addEventListener('keyup', NameListView.keyUp, false);
	}
	Arrows.hide();
}

NameListView.hide = function () {
	NameListView.style.visibility = "hidden";
	NameListView.active = 0;
	document.removeEventListener('keydown', NameListView.keyDown, false);
	document.removeEventListener('keypress', NameListView.keyPress, false);
	document.removeEventListener('keyup', NameListView.keyUp, false);
}

NameListView.clear = function () {
	var nameDiv = $('NameListSubDiv');
	if (nameDiv) {
		NameListView.element.removeChild(nameDiv);
	}
}

NameListView.refresh = function () {
    var nameDiv;
    var infoDiv;
	var nameList;
    var abCount = 0;
	
    nameList = AddressBookPlugin.nameList();
    abCount = nameList.length;

	// find old name sub div
	nameDiv = $('NameListSubDiv');
	if (nameDiv) {
		NameListView.element.removeChild(nameDiv);
	}
	   
	// Setup new div's
	nameDiv = document.createElement('div');
	nameDiv.setAttribute('id', 'NameListSubDiv');
	NameListView.element.appendChild(nameDiv);
		
    if (abCount == 0) {
        loadNoMatchTextDiv(nameDiv);
    } else {
		for (var i=0; i < abCount; i++) {
			gABCurrentCardIndex = NameListView.active = AddressBookPlugin.displayedItemIndex();
			var containerDiv = document.createElement('div');
			var valueDiv = document.createElement('div');
			var leftDiv  = document.createElement('div');
			var rightDiv = document.createElement('div');

			containerDiv.setAttribute('class', 'abNameContainerDiv');
			containerDiv.setAttribute('id', 'abNameContainerDiv'+i);
			
			valueDiv.setAttribute('class', 'abNameDiv');

			leftDiv.setAttribute('class', 'abNameDivLeft');
			rightDiv.setAttribute('class', 'abNameDivRight');

			if (i == NameListView.active) {
				// Set photo
				setCurrentPhotoFromPlugin();
				containerDiv.setAttribute('class', 'abNameContainerDiv abNameContainerDivSelected');
			}
			valueDiv.innerHTML = nameList[i];
			valueDiv.addEventListener('click', NameListView.mouseUp, false);
			valueDiv.setAttribute('cardIndex', i);
			
			containerDiv.appendChild(leftDiv);
			containerDiv.appendChild(valueDiv);
			containerDiv.appendChild(rightDiv);

			nameDiv.appendChild(containerDiv);
		}
	}
	
	// Update counts
	var cardNumberDiv = $('abCardNumberDiv');
	var found = getLocalizedString('@1 found');
	var count = AddressBookPlugin.count();
	
	found = found.replace("@1", "" + count);
	
	cardNumberDiv.innerHTML = found;
	Scroller.setView(nameDiv);
	if (count) {
		var element = $('abNameContainerDiv'+NameListView.active);
		if (element == null) {
			element = $('abNameContainerDiv' + 0);
		}
		Scroller.view.reveal(element);
	}
	Scroller.refresh();
	
	NameListView.updateNameCompletion();
}

function keyEventIsUpDownArrowKey(evt) {
	return evt.keyCode == 38 || evt.keyCode == 40;
}

NameListView.keyDown = function(evt) {
	if (keyEventIsUpDownArrowKey(evt)) {
		NameListView.abCount = AddressBookPlugin.count();
		NameListView.keyPress.isRepeat = false;
	}
}

NameListView.keyPress = function(evt) {
	if (evt.shiftKey || evt.metaKey || evt.altKey) {
		return;
	}
	
	var handled = false;
	var nameDiv = $('NameListSubDiv');
	var keyCode = evt.charCode;
	var oldIndex = NameListView.active;
	
	gSkipNextCompletion = false;
	
	if (keyCode == 27) { // esc
		$('SearchInputElement').value = "";
		View.clear();
		handled = true;
	}
	else if (keyCode == 63232) // up arrow
	{
		if (NameListView.active > 0)
			NameListView.active--;
		handled = true;
	}
	else if (keyCode == 63233) // down arrow
	{
		if (NameListView.active < (NameListView.abCount - 1))
			NameListView.active++;
		handled = true;
	}
	else if (keyCode == 8 || keyCode == 63272) // delete or backspace key
	{
		gSkipNextCompletion = true;
	}

	if (NameListView.active != oldIndex) {
		var searchField = $('SearchInputElement');
		var nameRecord = $('abNameContainerDiv'+NameListView.active);
		var fullName = nameRecord.innerText;
		var newValue = gSavedSearchString;
		if (fullName.toLowerCase().substring(0, gSavedSearchString.length) == gSavedSearchString) {
			newValue = gSavedSearchString + fullName.substring(gSavedSearchString.length);
		}
		searchField.value = newValue;
		searchField.setSelectionRange(gSavedSearchString.length, searchField.value.length);
		
		
		$('abNameContainerDiv'+oldIndex).className = 'abNameContainerDiv';
		nameRecord.className = 'abNameContainerDiv abNameContainerDivSelected';

		if (!NameListView.keyPress.isRepeat) {
			NameListView.keyPress.isRepeat = true;
			setCurrentPhotoToGeneric();
		}

		Scroller.view.reveal(nameRecord);
	}
	if (handled)
	{
		consumeEvent(evt);
	}
}

NameListView.keyUp = function(evt) {
	if (keyEventIsUpDownArrowKey(evt)) {
		AddressBookPlugin.displayCardAtIndex(NameListView.active);
		setCurrentPhotoFromPlugin();
		NameListView.updateNameCompletionEvenIfEmpty(true);
	}
}

NameListView.mouseUp = function (evt) {
	gABCurrentCardIndex = evt.currentTarget.getAttribute("cardIndex");
	View.setCurrent(CardDetailView);
	CardDetailView.refresh();
}

NameListView.dismiss = function () {
	if (View.getCurrent().viewKind == NameListView.viewKind) {
		gABCurrentCardIndex = NameListView.active;
		View.setCurrent(CardDetailView);
		CardDetailView.refresh();
	}
}

NameListView.updateNameCompletion = function () {
	NameListView.updateNameCompletionEvenIfEmpty(false);
}
NameListView.updateNameCompletionEvenIfEmpty = function (completeEvenIfEmpty) {
	if (!gSkipNextCompletion)
	{
		var searchField = $('SearchInputElement');
		var completion = AddressBookPlugin.completionStringForSearchString(gSavedSearchString);
		if (completeEvenIfEmpty || completion != "")
		{
			searchField.value = gSavedSearchString + completion;
			searchField.setSelectionRange(gSavedSearchString.length, searchField.value.length);
		}
	}
	gSkipNextCompletion = false;
}

/*======================================================================
 *	Card Detail View
 *======================================================================*/
CardDetailView = function () {
	var element = $('abCardInfoBlock');
	
    CardDetailView.detailBlock = $('abCardDetailBlock');
    CardDetailView.element = element;
	CardDetailView.style = element.style;
}
window.addEventListener('load', CardDetailView, false);

CardDetailView.resetForNewSearch = function () {
	CardDetailView.clear();
}

CardDetailView.viewKind = "DetailView";

CardDetailView.activate = function () {
	Arrows.show();
	document.addEventListener('keypress', CardDetailView.keyPress, false);
}

CardDetailView.hide = function () {
	var snapbackElement = $('SearchInputElementSnapbackIcon');
	if (snapbackElement != null)
	{
		snapbackElement.style.visibility = "hidden";
	}
	Arrows.hide();
	CardDetailView.style.visibility = "hidden";
	document.removeEventListener('keypress', CardDetailView.keyPress, false);
}

CardDetailView.keyPress = function(evt) {
	if (evt.charCode == 27) { // esc
		doSnapback();
		consumeEvent(evt);
	}
}

CardDetailView.clear = function () {
    var infoDiv;
    var detailTableWrapper;

    infoDiv = $('abCardInfoSubDiv');
    if (infoDiv) {
        CardDetailView.element.removeChild(infoDiv);  
    }
   
	detailTableWrapper = $('abCardDetailTableWrapper')
	if(detailTableWrapper)
		CardDetailView.detailBlock.removeChild(detailTableWrapper);
}

CardDetailView.refresh = function ()
{
    var detailTable;
    var detailTableWrapper;
    var infoDiv;
    var cardNumberDiv

	CardDetailView.clear()

	var snapbackElement = $('SearchInputElementSnapbackIcon');
	if (snapbackElement == null)
	{
		snapbackElement = document.createElement('img');

		snapbackElement.setAttribute('id', 'SearchInputElementSnapbackIcon');
		snapbackElement.setAttribute('onmousedown', 'snapbackMouseDown(event, this);');

		snapbackElement.setAttribute('src', 'Images/Search_SnapBack.png');
		
		// append the snapback button to the topContent div
		$('topContent').appendChild(snapbackElement);
	}
	snapbackElement.style.visibility = "visible";

    if (AddressBookPlugin.count()) {
		// Tell the model what we want to display

		AddressBookPlugin.displayCardAtIndex(gABCurrentCardIndex);
		
		// update the name/search field
		var searchField = $('SearchInputElement');
		searchField.value = AddressBookPlugin.displayedName();
		searchField.setSelectionRange(0, searchField.value.length);
		
		// find old detail sub div
		infoDiv = $('abCardInfoSubDiv');
		if (infoDiv) {
			CardDetailView.element.removeChild(infoDiv);  
		}
		
		detailTableWrapper = $('abCardDetailTableWrapper');
		if (detailTable) {
			CardDetailView.detailBlock.removeChild(detailTableWrapper);
		}
	   
		// Setup new div's
		infoDiv = document.createElement('div');
		infoDiv.setAttribute('id', 'abCardInfoSubDiv');
		CardDetailView.element.appendChild(infoDiv);
	
		detailTableWrapper = document.createElement('div');
		detailTableWrapper.id = "abCardDetailTableWrapper";

		detailTable = document.createElement('table')
		detailTable.id = "abCardDetailTable"
		detailTable.setAttribute("cellspacing", 0)

		detailTableWrapper.appendChild(detailTable)
		CardDetailView.detailBlock.appendChild(detailTableWrapper)
	
		if (gABCurrentCardIndex == 999999) {
			cardNumberDiv = $('abCardNumberDiv');
			cardNumberDiv.innerHTML = getLocalizedString('No Matching Cards');
			loadNoMatchTextDiv(infoDiv);
		} else {
			// Update counts
			cardNumberDiv = $('abCardNumberDiv');
			var beforeIndex = AddressBookPlugin.displayedItemIndex();
			var outOf = getLocalizedString('@1 of @2');
			
			CardDetailView.generateCardDetailTable(infoDiv, detailTable);
			gABCurrentCardIndex = AddressBookPlugin.indexOfItem();

			outOf = outOf.replace("@1", (gABCurrentCardIndex + 1));
			outOf = outOf.replace("@2", AddressBookPlugin.totalCount());
			
			cardNumberDiv.innerHTML = outOf;
		}

		Scroller.setView(detailTableWrapper);
		Scroller.refresh();
		Arrows.show();
	}
}

CardDetailView.generateCardDetailTable = function (infoDiv, detailTable) {
	var photoDiv;
	var propertyDiv;
	var itemCount;
    
    var currentProperty = null,
        previousProperty = null;
    var propertySeparator = null;
	
	// Set photo
	setCurrentPhotoFromPlugin();

	loadNameBlockDiv(infoDiv);
	
	itemCount = AddressBookPlugin.displayedItemCount();

	for (var i = 0; i < itemCount; i++) {
        currentProperty = AddressBookPlugin.displayedPropertyAtIndex(i);


		if (previousProperty != null && currentProperty != previousProperty && !CardDetailView.bothPropertiesAreInstantMessenger(previousProperty, currentProperty))
        {
            propertySeparator = document.createElement('tr');
            propertySeparator.setAttribute('class', 'abPropertySeparatorRow');
            detailTable.appendChild(propertySeparator);
        }

		propertyRow = propertyRowFor(currentProperty,
									   AddressBookPlugin.displayedScriptAtIndex(i),
									   AddressBookPlugin.displayedLabelAtIndex(i),
									   AddressBookPlugin.displayedFullLabelAtIndex(i),
									   AddressBookPlugin.displayedValueAtIndex(i),
									   i);
		detailTable.appendChild(propertyRow);
        previousProperty = currentProperty;
	}
	
	var adjustToWidth = itemCount ? $('label-0').clientWidth : null;
	CardDetailView.adjustForLabelsOfWidth(adjustToWidth);
}

var lastExtendBy = 0;

CardDetailView.adjustForLabelsOfWidth = function (width) {
	//adjust the picture
	//adjust the window size

	extendBy = width ? width-50 : 0;
	extendBy = Math.min(extendBy, 33);
	extendBy = Math.max(extendBy, 0);

	$('PictureDiv').style.paddingLeft = extendBy + "px";
	var newWidth = kMinimumWidth+extendBy;
	Window.resizeTo(new Position(newWidth,Window.currentSize().y),"expanding");	
	$('FrontWindow').style.width = newWidth + "px";

	gCurrentWidth = newWidth;
}

CardDetailView.generateCardDetailDiv = function (infoDiv, detailDiv) {
	var photoDiv;
	var propertyDiv;
	var itemCount;
    
    var currentProperty = null,
        previousProperty = null;
    var propertySeparator = null;
	
	// Set photo
	setCurrentPhotoFromPlugin();

	loadNameBlockDiv(infoDiv);
	
	itemCount = AddressBookPlugin.displayedItemCount();

	for (var i = 0; i < itemCount; i++) {
        currentProperty = AddressBookPlugin.displayedPropertyAtIndex(i);
		if (previousProperty != null && currentProperty != previousProperty && !CardDetailView.bothPropertiesAreInstantMessenger(previousProperty, currentProperty))
        {
            propertySeparator = document.createElement('div');
            propertySeparator.setAttribute('class', 'abPropertySeparatorDiv');
            
            detailDiv.appendChild(propertySeparator);
        }

		propertyDiv = propertyBlockFor(currentProperty,
									   AddressBookPlugin.displayedScriptAtIndex(i),
									   AddressBookPlugin.displayedLabelAtIndex(i),
									   AddressBookPlugin.displayedFullLabelAtIndex(i),
									   AddressBookPlugin.displayedValueAtIndex(i),
									   i);
		detailDiv.appendChild(propertyDiv);
        previousProperty = currentProperty;
	}
}

CardDetailView.bothPropertiesAreInstantMessenger = function (firstProperty, secondProperty) {
	var firstIsIM = CardDetailView.isInstantMessenger(firstProperty);
	var secondIsIM = CardDetailView.isInstantMessenger(secondProperty);
	
	return firstIsIM && secondIsIM;
}

CardDetailView.isInstantMessenger = function (property) {
	return property == "AIMInstant" || property == "JabberInstant" || property == "ICQInstant" || property == "MSNInstant" || property == "YahooInstant";
}

/*======================================================================
 *	Title View
 *======================================================================*/
TitleView = function () {
	var element = $('title');

	TitleView.element = element;
	TitleView.style = element.style;
}
window.addEventListener('load', TitleView, false);

TitleView.resetForNewSearch = function () {
}

TitleView.viewKind = "TitleView";

TitleView.activate = function () {
	// noop
}

TitleView.refresh = function () {
	// noop
}

TitleView.hide = function () {
	TitleView.style.visibility = "hidden";
}

/*======================================================================
 *	Arrows
 *======================================================================*/
Arrows = function () {
	Arrows.lArrow = $('abLeftButtonDiv');
	Arrows.rArrow = $('abRightButtonDiv');
	Arrows.trackingButton = null;
	Arrows.lArrow.addEventListener("mousedown", Arrows.mouseDown, false);
	Arrows.lArrow.addEventListener("mouseover", Arrows.mouseOver, false);
	Arrows.lArrow.addEventListener("mouseout", Arrows.mouseOut, false);
	Arrows.rArrow.addEventListener("mousedown", Arrows.mouseDown, false);
	Arrows.rArrow.addEventListener("mouseover", Arrows.mouseOver, false);
	Arrows.rArrow.addEventListener("mouseout", Arrows.mouseOut, false);
}
window.addEventListener('load', Arrows, false);

Arrows.hide = function () {
	Arrows.lArrow.style.visibility = 'hidden';
	Arrows.rArrow.style.visibility = 'hidden';
}

Arrows.show = function () {
	if (View.getCurrent().viewKind == CardDetailView.viewKind) {
		var parentVis = $("pageBottomContent").style.visibility;
		Arrows.lArrow.style.visibility = parentVis;
		Arrows.rArrow.style.visibility = parentVis;
	}
}

Arrows.trackMouse = function (evt) {
	// we use this event listener for the global document instead of an event 
	// handler on the button <IMG> so that we can know to stop the tracking
	// process even if the mouseup happens outside of the button
	
	if (null != Arrows.trackingButton)
		Arrows.trackingButton.src = "Images/"+Arrows.trackingButton.getAttribute("tag")+".png";
	
	// note that evt.target will point to the IMG object only if this is a 
	// mouseup inside the button, and also make sure we don't respond to mouseup
	// in another button, hence the extra check for this event being inside the 
	// button being tracked
	
	if (evt.target == Arrows.trackingButton)
	{
		if (evt.target.getAttribute("tag") == "Previous")
		{
			Arrows.clickedLeftButton(evt);
		}
		else if (evt.target.getAttribute("tag") == "Next")
		{
			Arrows.clickedRightButton(evt);
		}
	}
	
	consumeEvent(evt);
	
	document.removeEventListener ("mouseup", Arrows.trackMouse, false);
	Arrows.trackingButton = null;
}

Arrows.mouseDown = function (evt) {
	var element = evt.currentTarget;
	evt.target.src = "Images/"+element.getAttribute("tag")+" Pressed.png";
	consumeEvent(evt);
	document.addEventListener ("mouseup", Arrows.trackMouse, false);
	Arrows.trackingButton = element;
}

Arrows.mouseOut = function (evt) {
	var element = evt.currentTarget;
	evt.target.src = "Images/"+element.getAttribute("tag")+".png";
}

Arrows.mouseOver = function (evt) {
	var element = evt.currentTarget;
	if (evt.currentTarget == Arrows.trackingButton)
		evt.target.src = "Images/"+element.getAttribute("tag")+" Pressed.png";
}

Arrows.clickedLeftButton = function (evt) {
    if (gABCurrentCardIndex == 999999) {
        return;
    }
    gABCurrentCardIndex--;

	View.setCurrent(CardDetailView);
    if (gABCurrentCardIndex < 0) {
        var max = AddressBookPlugin.totalCount();
        gABCurrentCardIndex = max - 1;
    }
        
    CardDetailView.refresh();
}

Arrows.clickedRightButton = function (evt) {
    var max = AddressBookPlugin.totalCount();
    
    if (gABCurrentCardIndex == 999999) {
        return;
    }
    
	View.setCurrent(CardDetailView);
    gABCurrentCardIndex++;

    if (gABCurrentCardIndex >= max)
        gABCurrentCardIndex = 0;
        
    CardDetailView.refresh();
}

/*======================================================================
 *	Preference & Localization Utilities
 *======================================================================*/
var Preference = new Object();
Preference.instancePrefs = new Array();

Preference.get = function (key) {
	if (window.widget) {
		var str = widget.preferenceForKey(_createkey(key));
		if ( str == null) {
			str = widget.preferenceForKey(key);
			if ( str == null) {
				str = localizedStrings[key];
			}
		}
		return str;
	}
}

Preference.set = function (key,value) {
	if (window.widget) {
		var instanceKey = _createkey(key);
		widget.setPreferenceForKey( value,  instanceKey);
		widget.setPreferenceForKey( value,  key);
		Preference.instancePrefs[instanceKey] = true;
	}
}

Preference.removeInstancePreferences = function () {
	if (window.widget) {
		for ( key in Preference.instancePrefs ) {
			widget.setPreferenceForKey(null, key);
		}
	}
}

function _createkey(key) {
        return widget.identifier + "-" + key;
}

function getLocalizedString (key)
{
	try {
		return localizedStrings[key];
	} catch (ex) {}

	return key;
}

/*======================================================================
 *	Tracking Mouse
 *======================================================================*/
TrackMouse = function (view) {
	if ( ! TrackMouse.tracking) {
		TrackMouse.tracking = true;
		view.highlight(true);
		TrackMouse.element = view.element;
		TrackMouse.view    = view;
		TrackMouse.mouseWithin = true;

		TrackMouse.element.addEventListener("mouseover", TrackMouse.mouseOver, false);
		TrackMouse.element.addEventListener("mouseout",  TrackMouse.mouseOut,  false);
		document.addEventListener("mouseup",  TrackMouse.mouseUp,   true);
	}
}

TrackMouse.mouseOver = function (evt) {
	TrackMouse.mouseWithin = true;
	TrackMouse.view.highlight(true);
}

TrackMouse.mouseOut = function (evt) {
	TrackMouse.mouseWithin = false;
	TrackMouse.view.highlight(false);
}

TrackMouse.mouseUp = function (evt) {
	TrackMouse.tracking = false;
	TrackMouse.element.removeEventListener("mouseover", TrackMouse.mouseOver, false);
	TrackMouse.element.removeEventListener("mouseout",  TrackMouse.mouseOut,  false);
	document.removeEventListener("mouseup",  TrackMouse.mouseUp,   true);
	TrackMouse.view.highlight(false);
	if ( TrackMouse.mouseWithin)
		TrackMouse.view.clicked();
}

/*======================================================================
 *	UTILITIES
 *======================================================================*/
function max(a,b) { return (a>b) ? a : b; }
function min(a,b) { return (a<b) ? a : b; }
function abs(a)   { return (a>0) ? a : -a; }

/*======================================================================
 *	Position object
 *======================================================================*/
var Position = function (x,y) { this.x = x; this.y = y;}
Position.prototype = {
set : function (pos) { this.x = pos.x; this.y = pos.y;},
add : function (pos) { return new Position(this.x+pos.x, this.y+pos.y);},
sub : function (pos) { return new Position(this.x-pos.x, this.y-pos.y);},
equal : function (pos) { return (this.x==pos.x) && (this.y==pos.y); },
toString : function () { return "["+this.x+","+this.y+"]";}
}

/*======================================================================
 *  Debug
 *======================================================================*/

function debug(msg) {
     if (!debug.box) {
          debug.box = document.createElement('div');
          debug.box.setAttribute('style', 'background-color: white; ' +
                                                  'font-family: monospace; ' +
                                                  'border: solid black 3px; ' +
                                                  'padding: 10px;');
          document.body.appendChild(debug.box);
          debug.box.innerHTML = '<h1 style="text-align:center">Debug Output</h1>';
     }
     
     var p = document.createElement('p');
     p.appendChild(document.createTextNode(msg));
     debug.box.appendChild(p);
}

/*======================================================================
 *  UI Building
 *======================================================================*/


function propertyRowFor (property, script, label, fullLabel, value, index)
{
	var enclosingDiv;
	var labelDiv;
	var valueDiv;

	newRow = document.createElement('tr');
	newRow.className = "abPropertyRow";
	
	labelCell = document.createElement('td')
	labelCell.className = "abLabelCell"
	labelCell.width = "48px"
	labelCell.id= 'label-' + index;

	var html = label;
	if (script)
		html = "<A HREF=\"#\" onMouseOut=\"unhighlight(" + index + ");\" onMouseOver=\"highlight(" + index + ");\" onClick=\"" + script + "\">" + label + "</A>";
	labelCell.innerHTML = html;

	valueCell = document.createElement('td');
	valueCell.setAttribute('class', 'abValueCell');
	valueCell.setAttribute('id', 'value-' + index);

	html = value;
	if (script)
		html = "<A HREF=\"#\" onMouseOut=\"unhighlight(" + index + ");\" onMouseOver=\"highlight(" + index + ");\" onClick=\"" + script + "\">" + value + "</A>";

    // add the service to the instant messenger properties
    if (property == "AIMInstant" || (property == "JabberInstant") || (property == "MSNInstant") || (property == "YahooInstant") || (property == "ICQInstant"))
        html = getLocalizedString(property).replace("@1", html);

	valueCell.innerHTML = html;

	newRow.appendChild(labelCell);
	newRow.appendChild(valueCell);
	return newRow;
}


// Loads the interactive bits of the card:
// Returns a div containing the label and value
function propertyBlockFor (property, script, label, fullLabel, value, index)
{
	var enclosingDiv;
	var labelDiv;
	var valueDiv;
	
	enclosingDiv = document.createElement('div');
	enclosingDiv.setAttribute('class', 'abPropertyDiv');
	
	labelDiv = document.createElement('div');
	labelDiv.setAttribute('class', 'abLabelDiv');
	labelDiv.setAttribute('id', 'label-' + index);
	
	var html = label;
	if (script)
		html = "<A HREF=\"#\" onMouseOut=\"unhighlight(" + index + ");\" onMouseOver=\"highlight(" + index + ");\" onClick=\"" + script + "\">" + label + "</A>";
	labelDiv.innerHTML = html;
	
	valueDiv = document.createElement('div');
	valueDiv.setAttribute('class', 'abValueDiv');
	valueDiv.setAttribute('id', 'value-' + index);
	
	html = value;
	if (script)
		html = "<A HREF=\"#\" onMouseOut=\"unhighlight(" + index + ");\" onMouseOver=\"highlight(" + index + ");\" onClick=\"" + script + "\">" + value + "</A>";
    
    // add the service to the instant messenger properties
    if (property == "AIMInstant" || (property == "JabberInstant") || (property == "MSNInstant") || (property == "YahooInstant") || (property == "ICQInstant"))
        html = getLocalizedString(property).replace("@1", html);
    
	valueDiv.innerHTML = html;
	
	enclosingDiv.appendChild(labelDiv);
	enclosingDiv.appendChild(valueDiv);
	return enclosingDiv;     
}

function updateTextDecorationAtIndex(index, decoration)
{
	var labelElement = $("label-" + index);
	labelElement.style.textDecoration = decoration;
	var valueElement = $("value-" + index);
	valueElement.style.textDecoration = decoration;
}

function highlight(index)
{
	updateTextDecorationAtIndex(index, "underline");
}

function unhighlight(index)
{
	updateTextDecorationAtIndex(index, "none");
}

function truncateElementHTML(element, maxHeight)
{
	if (element.offsetHeight <= maxHeight)
		return;
	
    var string = element.firstChild.innerHTML;
    var len;
    for (len = string.length; len > 0; len--)
    {
        string = string.substring(0, len);
        element.firstChild.innerHTML = string + "&hellip;";
		if (element.offsetHeight <= maxHeight)
			return;
    }
}

function truncateElementText(element, maxHeight)
{
	if (element.offsetHeight <= maxHeight)
		return;
	
    var string = element.innerText;
    var len;
    for (len = string.length; len > 0; len--)
    {
        string = string.substring(0, len);
        element.innerHTML = string + "&hellip;";
		if (element.offsetHeight <= maxHeight)
			return;
    }
}

// Adds the Name and Org fields to the card
function loadNameBlockDiv (textDiv)
{
	var tr;
	var altNameDiv;
	altNameDiv = document.createElement('div');
	altNameDiv.setAttribute('id', 'abPersonAltName');
	
	textDiv.appendChild(altNameDiv);
	
	var altName = AddressBookPlugin.displayedAltName();
	if (altName == null)
		altName = "";
	
	altNameDiv.innerText = altName;
}

function loadNoMatchTextDiv (textDiv) 
{
	var noMatchDiv = document.createElement('div');
	noMatchDiv.setAttribute('id', 'abNoMatchDiv');
	noMatchDiv.innerText = getLocalizedString('No Matching Cards');
	textDiv.appendChild(noMatchDiv);
	setCurrentPhotoToGeneric();
}

function setCurrentPhotoToGeneric() {
	setCurrentPhoto("Images/PersonSquare.png");
}

function setCurrentPhotoFromPlugin() {
	setCurrentPhoto(AddressBookPlugin.displayedPhoto());
}

function setCurrentPhoto(path) {
	var photoDiv = $('PictureDiv');
	photoDiv.src = path;
}



/*======================================================================
 *  Event Handlers
 *======================================================================*/

var snapbackIconMouseOver = false;
var snapbackIconMouseDown = false;

function snapbackMouseDown (evt, img)
{
	snapbackIconMouseDown = true;
	
	var element = $('SearchInputElementSnapbackIcon');
	element.src = 'url("Images/Search_SnapBackPressed.png")';

	document.addEventListener("mouseup", snapbackMouseUp, false);
	document.addEventListener("mousemove", snapbackMouseMove, false);
	element.addEventListener ("mouseover", snapbackMouseOver, false);
	element.addEventListener ("mouseout", snapbackMouseOut, false);
	
	element.mouseInButton = true;
	
	consumeEvent(evt);
}

function snapbackMouseUp (evt)
{
	var element = $('SearchInputElementSnapbackIcon');

	document.removeEventListener ("mouseup", snapbackMouseUp, false);
	document.removeEventListener ("mousemove", snapbackMouseMove, false);
	element.removeEventListener ("mouseover", snapbackMouseOver, false);
	element.removeEventListener ("mouseout", snapbackMouseOut, false);

	if (element.mouseInButton)
	{
		element.src = 'url("Images/Search_SnapBack.png")';
		doSnapback();
	}
	element.mouseInButton = false;
	consumeEvent(evt);
}

function doSnapback()
{
	var searchField = $('SearchInputElement');
	searchField.focus();
	searchField.value = gSavedSearchString;
	// make sure the carret is at the end
	searchField.setSelectionRange(gSavedSearchString.length, gSavedSearchString.length);
	SearchField.performSearchAndSelectBestMatch(gSavedSearchString, false);
	View.newSearch();
	CardDetailView.adjustForLabelsOfWidth(null);
}

function snapbackMouseMove (evt)
{
	// do nothing
	consumeEvent(evt);
}

function snapbackMouseOver (evt)
{
	var element = $('SearchInputElementSnapbackIcon');
	
	if (!element.mouseInButton)
	{
		element.src = 'Images/Search_SnapBackPressed.png';
		element.mouseInButton = true;
	}
	consumeEvent(evt);
}

function snapbackMouseOut (evt)
{
	var element = $('SearchInputElementSnapbackIcon');
	
	if (element.mouseInButton)
	{
		element.src = 'Images/Search_SnapBack.png';
		element.mouseInButton = false;
	}

	consumeEvent(evt);
}

function keyPressed (evt)
{
    var handled = false;
	
    if (evt.metaKey) // command is down
    {
        var keyString = String.fromCharCode(evt.charCode);
        if (keyString == '[')
        {
            Arrows.clickedLeftButton (evt);
            handled = true;
        }
        else if (keyString == ']')
        {
            Arrows.clickedRightButton (evt);
            handled = true;
        }
    }
    else if (evt.keyCode == 13 || evt.keyCode == 3) // return or enter
    {
		if (View.isCurrentViewDetailView())
		{
			if (evt.shiftKey)
				Arrows.clickedLeftButton (evt);
			else
				Arrows.clickedRightButton (evt);
			handled = true;
		}
		else if (View.isCurrentViewNameListView())
		{
			//if it's not animating, and if a new search hasn't been started (4385197)
			if (!ExpandShrink._animation && $('SearchInputElement').value.indexOf(gSavedSearchString) == 0)
			{
				gABCurrentCardIndex = NameListView.active;
				View.setCurrent(CardDetailView);
				CardDetailView.refresh();
			}
			handled = true;
		}
    }
    else if (evt.keyCode == 9) // tab key
	{
		var searchField = $('SearchInputElement');
		var selectionStart = searchField.selectionStart,
			selectionEnd   = searchField.selectionEnd;
		searchField.focus();
		searchField.setSelectionRange(selectionStart, selectionEnd);
		
		handled = true;
	}
    else if (View.isCurrentViewNameListView())
	{
		if (!ExpandShrink._animation)
		{
	    	NameListView.keyPress(evt);
	    }
	}
	
    if (handled)
    {
		consumeEvent(evt);
    }
}
document.addEventListener("keypress", keyPressed, true);


/*======================================================================
 *	Debugging
 *======================================================================*/
function LOG__(arg)
{
	if (window.widget && gDisplayLog)
		alert(":"+arg);
	else if (gDisplayLog)
		debug(arg);
}

/*======================================================================
 *	End of the script
 *======================================================================*/

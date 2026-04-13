var stretcher;
var categoryScroller;
var filterScroller;
var filterInfoScroller;
var filterPreviewScroller;
var imagePreviewArray = new Array();
var currentCategorySelection = null;
var debugMode = false;

if(window.widget)
{
	widget.onreceiverequest = receiverequest;
}

function DEBUG(str) 
{
	if(debugMode) 
	{
		if(window.widget)
		{
			alert(str);
		} else {
			
			var debugDiv = document.getElementById('debugDiv');
			debugDiv.appendChild(document.createTextNode(str));
			debugDiv.appendChild(document.createElement("br"));
			debugDiv.scrollTop = debugDiv.scrollHeight;
		}
	}
	
}

function toggleDebug()
{
	debugMode = !debugMode;
	if(debugMode == true && !window.widget)
		document.getElementById('debugDiv').style.display = 'block';
	else
		document.getElementById('debugDiv').style.display = 'none';
}

function removeAllChildren (parent)
{
	while (parent.hasChildNodes())
		parent.removeChild(parent.firstChild);
}

function createCategoryRow (object, title)
{				
	var row = document.createElement('tr');
	
	row.setAttribute ("class", "stockRow");
	row.setAttribute ("onclick", "clickoncategoryrow(event, this);");
	row.setAttribute ("tag", object);
	
	var td = document.createElement ('td');
	td.setAttribute ("class", "listEntry");
	td.appendChild (document.createTextNode(title));
	row.appendChild (td);

	return row;
}

function createFilterRow (object, title)
{				
	var row = document.createElement('tr');
	
	row.setAttribute ("onclick", "clickonfilterrow(event, this);");
	row.setAttribute ("tag", object);
	row.setAttribute ("id", object);
	
	var td = document.createElement ('td');
	if(CIPlugin.markedFilter(object))
		td.setAttribute ("class", "taggedListEntry");
	else
		td.setAttribute ("class", "listEntry");
	td.appendChild (document.createTextNode(title));
	row.appendChild (td);
		
	return row;
}


function changeSelection(oldSelection, currentSelection)
{
	if (oldSelection != null)
	{
		oldSelection.children[0].setAttribute ("class", "listEntry");
		
	}
	currentSelection.children[0].setAttribute ("class", "listEntrySelected");
}

function changeFilterSelection(oldSelection, currentSelection)
{
	if (oldSelection != null)
	{
		if(CIPlugin.markedFilter(oldSelection.getAttribute("tag")))
			oldSelection.children[0].setAttribute ("class", "taggedListEntry");
		else
			oldSelection.children[0].setAttribute ("class", "listEntry");		
	}
	if(CIPlugin.markedFilter(currentSelection.getAttribute("tag")))
		currentSelection.children[0].setAttribute ("class", "taggedListEntrySelected");
	else
		currentSelection.children[0].setAttribute ("class", "listEntrySelected");
}

function clickoncategoryrow (event, row)
{
	var oldSelection = currentCategorySelection;
	
	currentCategorySelection = row;
	if (window.widget && oldSelection != currentCategorySelection)
	{
		//write out the selection
		//widget.setPreferenceForKey (row.getAttribute("tag"), createKey("categorySelection"));
	}
	
	changeSelection(oldSelection, currentCategorySelection);
	showFiltersContent(currentCategorySelection.getAttribute("tag"));

}

function clickonfilterrow (event, row)
{
	var oldSelection = currentFilterSelection;
	
	currentFilterSelection = row;
	if (window.widget && oldSelection != currentFilterSelection)
	{
		//write out the selection
		//widget.setPreferenceForKey (row.getAttribute("tag"), createKey("categorySelection"));
	}
	
	changeFilterSelection(oldSelection, currentFilterSelection);
	showInfoForFilter(row.getAttribute("tag"));

}

function createInfoRowHeader (key, object, type)
{				
	var table = document.createElement('table');
	var tableBody = document.createElement('tbody');
	var innerrow = document.createElement('tr');
	var row = document.createElement('tr');
		
	var td1 = document.createElement ('td');
	td1.setAttribute ("class", "infoElementTitle");
	td1.setAttribute ("width", "100px");
	td1.appendChild (document.createTextNode(key));
	innerrow.appendChild (td1);
	
	var td2 = document.createElement ('td');
	td2.setAttribute ("class", "infoElement");
	td2.setAttribute ("width", "100px");
	td2.appendChild (document.createTextNode(object));
	innerrow.appendChild (td2);
	
	var td3 = document.createElement ('td');
	td3.setAttribute ("class", "infoElement");
	td3.setAttribute ("width", "100px");
	td3.appendChild (document.createTextNode(type));
	innerrow.appendChild (td3);

	tableBody.appendChild (innerrow);

	table.setAttribute ("border", "0");
	table.setAttribute ("cellspacing", "0");
	table.setAttribute ("cellpadding", "0");
	table.appendChild (tableBody);
	row.appendChild (table);
	return row;
}

function createInfoRowElement (key, object)
{				
	var table = document.createElement('table');
	var tableBody = document.createElement('tbody');
	var innerrow = document.createElement('tr');
	var row = document.createElement('tr');
		
	var td = document.createElement ('td');
	td.setAttribute ("class", "infoElementSubTitle");
	td.setAttribute ("width", "10px");
	//td.appendChild (document.createTextNode("-"));
	innerrow.appendChild (td);
	
	var td1 = document.createElement ('td');
	td1.setAttribute ("class", "infoElementSubTitle");
	td1.setAttribute ("width", "140px");
	td1.appendChild (document.createTextNode(key));
	innerrow.appendChild (td1);
	
	var td2 = document.createElement ('td');
	td2.setAttribute ("class", "infoElement");
	td2.setAttribute ("width", "150px");
	td2.appendChild (document.createTextNode(object));
	innerrow.appendChild (td2);

	tableBody.appendChild (innerrow);

	table.setAttribute ("border", "0");
	table.setAttribute ("cellspacing", "0");
	table.setAttribute ("cellpadding", "0");
	table.appendChild (tableBody);
	row.appendChild (table);
	return row;
}

function createImageRow (inputImageKey)
{				
	var row = document.createElement('tr');
	var	imageField = document.createElement("img")
	
	var td = document.createElement ('td');
	td.setAttribute ("class", "listEntry");
	td.appendChild (document.createTextNode(inputImageKey));
	td.appendChild (document.createElement("br"));
	
	imageField.setAttribute ("tag", inputImageKey);
	imageField.setAttribute ("id", inputImageKey);
	imageField.setAttribute ("width", "110px");
	imageField.setAttribute ("height", "110px");
	if (imagePreviewArray[inputImageKey] == null) {
		imageField.setAttribute ("src", "tiger.jpg");
	} else {
		// setup the filter preview
		var plugin = document.embeds["CIPluginPreview"];
		imageField.setAttribute ("src", imagePreviewArray[inputImageKey]);
		plugin.setImageByPath(imagePreviewArray[inputImageKey], inputImageKey);
	}
	imageField.setAttribute ("ondrop", "dragdrop(\'" + inputImageKey + "\', event)");
	imageField.setAttribute ("ondragenter", "dragenter(event)");
	imageField.setAttribute ("ondragover", "dragover(event)");
	imageField.setAttribute ("ondragleave", "dragleave(event)");
	td.appendChild (imageField);
	row.appendChild (td);
	row.setAttribute ("height", "125px");
	return row;
}

function insertSpacerRow(inContainer)
{
	var row = document.createElement('tr');
	td = document.createElement ('td');
	td.setAttribute ("class", "infoSpacer");
	row.appendChild (td);
	inContainer.appendChild (row);
}

function showFilterInfoContent(inContainer) {
		
	// Resize, reposition the WidgetScroller thumb.
	// ***IMPORTANT! This needs to be called EVERY TIME you change the content to be scrolled.
	var scrollBar	= document.getElementById('filterInfoScroller');
	var scrollThumb = document.getElementById('filterInfoScrollThumb');
	var scrollTrack = document.getElementById('filterInfoScrollTrack');

	filterInfoScroller = new Scroller(inContainer, scrollBar, scrollThumb, scrollTrack);
		
}	

function showFilterPreviewContent(inContainer) {
		
	// Resize, reposition the WidgetScroller thumb.
	// ***IMPORTANT! This needs to be called EVERY TIME you change the content to be scrolled.
	var scrollBar	= document.getElementById('filterPreviewScroller');
	var scrollThumb = document.getElementById('filterPreviewScrollThumb');
	var scrollTrack = document.getElementById('filterPreviewScrollTrack');

	filterPreviewScroller = new Scroller(inContainer, scrollBar, scrollThumb, scrollTrack);
		
}	
function receiverequest (request)
{
	var filter = request['filter'];
	DEBUG(filter);
	if (filter)
	{
		searchForFilterByName(filter);
	}
}

function showFilterRefDocForFilter(filterName)
{
	if (window.widget)
	{
		widget.openURL(CIPlugin.localizedReferenceDocumentationForFilterName(filterName));
	}

}

function showFilterRefDoc()
{
	showFilterRefDocForFilter(currentFilterSelection.getAttribute("tag"));
}

function showInfoForFilter(filterName)
{
	var filterTitle = document.getElementById('filterTitle');
	var filterSubTitle = document.getElementById('filterSubTitle');
	var container ;
		
	if(filterName == null) {
		
		filterTitle.setAttribute ("class", "filterTitle");
		filterTitle.innerHTML = 'Filter not found';
		filterSubTitle.innerHTML = '';
		categories.innerHTML = '';	
		container = document.getElementById('categories');
		removeAllChildren(container);
		container = document.getElementById('inputKeystbody');
		removeAllChildren(container);
		showFilterInfoContent(document.getElementById('filterInfoList'));
		container = document.getElementById('inputImagetbody');
		removeAllChildren(container);
		showFilterPreviewContent(document.getElementById('filterPreviewList'));
		
	} else {
		var i;
		var	count;
		var row = null;
		var td = null;
		var attributes = CIPlugin.coalescedInfoForFilterName(filterName);
		var locName = CIPlugin.localizedNameForFilterName(filterName);
		
		if(locName.length > 20)
			filterTitle.setAttribute ("class", "filterTitleSmall");
		else
			filterTitle.setAttribute ("class", "filterTitle");
		filterTitle.innerHTML = locName;
		filterSubTitle.innerHTML = filterName;
		
		container = document.getElementById('availabilityInformation');
		container.innerHTML = CIPlugin.availabilityInformationForFilter(filterName);
		
		locName = CIPlugin.localizedDescriptionForFilterName(filterName);
		if(locName)
		{
			container = document.getElementById('filterDescription');
			container.innerHTML = locName;
		}
		
		categories.innerHTML = attributes[0];
	
		container = document.getElementById('categories');
		
		removeAllChildren(container);
		count = attributes[0].length;
		for(i = 0; i < count; i++)
		{
			row = document.createElement('tr');
			td = document.createElement ('td');
			td.setAttribute ("class", "infoElement");
			td.appendChild (document.createTextNode(attributes[0][i]));
			row.appendChild (td);
			container.appendChild (row);
		}
		
		container = document.getElementById('inputKeystbody');
		
		removeAllChildren(container);
		count = attributes.length;
		for(i = 1; i < count; i++)
		{
			row = createInfoRowHeader(attributes[i][1], attributes[i][0], attributes[i][2]);
			container.appendChild (row);
			
			var	infoCount = attributes[i].length;
			for(var x = 3; x < infoCount; x++)
			{
				row = createInfoRowElement(attributes[i][x], attributes[i][++x]);
				container.appendChild (row);
			}
			insertSpacerRow(container);
	
		}
		showFilterInfoContent(document.getElementById('filterInfoList'));
		
		
		// setup the filter preview
		var plugin = document.embeds["CIPluginPreview"];
		plugin.setFilterByName(filterName);
		plugin.setImageByPath(document.CIPluginPreview.src , 'inputImage');
		
		var	inputImgages = CIPlugin.inputImageKeysForFilterName(filterName);
		container = document.getElementById('inputImagetbody');
		
		removeAllChildren(container);
		count = inputImgages.length;
		if(count > 0)
		{
			for(i = 0; i < count; i++)
			{
				row = createImageRow(inputImgages[i]);
				container.appendChild (row);
			}
		} else {
			
			document.getElementById('CIPluginPreview').style.height = "181px";	// to force the view to redraw
		}
		showFilterPreviewContent(document.getElementById('filterPreviewList'));
		
		// setup the filter preview
		plugin.setFilterByName(filterName);
		plugin.setImageByPath(document.CIPluginPreview.src , 'inputImage');
		document.getElementById('CIPluginPreview').style.height = "180px";
	}
}

function searchForFilterByName(searchString)
{
	var		foundFilters = new Array();	//array to store the filters we found
	var		allFilters = CIPlugin.getFiltersInCategory("CICategoryAll");
	var 	count = allFilters.length;
	var		searchPattern = new RegExp(searchString, "i");
	
	for(var i= 0; i < count; i++)
	{
		if( (searchPattern.test(allFilters[i])) || (searchPattern.test(CIPlugin.localizedNameForFilterName(allFilters[i]))) )
			foundFilters.push(allFilters[i]);
	}
		// Resize, reposition the WidgetScroller thumb.
	// ***IMPORTANT! This needs to be called EVERY TIME you change the content to be scrolled.
	var scrollBar	= document.getElementById('filterScroller');
	var scrollThumb = document.getElementById('filterScrollThumb');
	var scrollTrack = document.getElementById('filterScrollTrack');

	var c = foundFilters.length;
	var container = document.getElementById('filtertbody');
	removeAllChildren(container);	
		
	if(foundFilters.length > 0)
	{
		
	
		for (var i=0; i<c; ++i)	
	
		{
			var data = foundFilters[i];
			var row = createFilterRow (data, CIPlugin.localizedNameForFilterName(data));
			container.appendChild (row);
		}
		changeSelection(null, container.children[0]);
		currentFilterSelection = container.children[0];
		showInfoForFilter(currentFilterSelection.getAttribute("tag"));
	
			
	} else {
		showInfoForFilter(null);
	}
	filterScroller = new Scroller(document.getElementById('filterList'), scrollBar, scrollThumb, scrollTrack);
}

function searchForFilter()
{
	var		foundFilters = new Array();	//array to store the filters we found
	var		allFilters = CIPlugin.getFiltersInCategory("CICategoryAll");
	var 	count = allFilters.length;
	var		searchString = document.getElementById('filterSearch').value;

	searchForFilterByName(searchString);
}

function setup()
{

	toggleDebug();

	if(window.widget)		// always check to make sure that you are running in Dashboard
	{
		//setup the stretcher
		var mainElt = document.getElementById('main');
		stretcher = new Stretcher(mainElt, 200, null); //function() { DEBUG("new height=" + mainElt.style.height); });
		
		
	}
	imagePreviewArray.inputTexture = "Images/smoothtexture.png";
	imagePreviewArray.inputShadingImage = "Images/smoothtexture.png";
	imagePreviewArray.inputGradientImage = "Images/colormap.png";
	imagePreviewArray.inputMaskImage = "Images/mask.png";
	
	//setup the scrollers
	showCategoriesContent();
	//setup the scrollers
	showFiltersContent("CICategoryAll");
	
	var plugin = document.embeds["CIPluginPreview"];
	plugin.setFilterByName('CICheckerboardGenerator');

}

// Switches between the content DIVs.
function showCategoriesContent() {
		
	// Resize, reposition the WidgetScroller thumb.
	// ***IMPORTANT! This needs to be called EVERY TIME you change the content to be scrolled.
	var scrollBar	= document.getElementById('categoryScroller');
	var scrollThumb = document.getElementById('categoryScrollThumb');
	var scrollTrack = document.getElementById('categoryScrollTrack');
	
	var categories = CIPlugin.getAllCategories();	
	var c = categories.length;
	var container = document.getElementById('categorytbody');
	removeAllChildren(container);	
	

	// add the categories
	var row;
	for (var i=0; i<c; ++i)	

	{
		var data = categories[i];
		row = createCategoryRow (data, CIPlugin.localizedNameForCategory(data));
		container.appendChild (row);
	}
	row = container.children[0];
	changeSelection(null, row);
	currentCategorySelection = row;

	categoryScroller = new Scroller(document.getElementById('categoryList'), scrollBar, scrollThumb, scrollTrack);
		
}	

function showFiltersContent(inCategory) {
		
	// Resize, reposition the WidgetScroller thumb.
	// ***IMPORTANT! This needs to be called EVERY TIME you change the content to be scrolled.
	var scrollBar	= document.getElementById('filterScroller');
	var scrollThumb = document.getElementById('filterScrollThumb');
	var scrollTrack = document.getElementById('filterScrollTrack');

	filters = CIPlugin.getFiltersInCategory(inCategory);	
	var c = filters.length;
	var container = document.getElementById('filtertbody');
	removeAllChildren(container);	
	

	for (var i=0; i<c; ++i)	

	{
		var data = filters[i];
		var row = createFilterRow (data, CIPlugin.localizedNameForFilterName(data));
		container.appendChild (row);
	}
	changeFilterSelection(null, container.children[0]);
	currentFilterSelection = container.children[0];
	showInfoForFilter(currentFilterSelection.getAttribute("tag"));

	filterScroller = new Scroller(document.getElementById('filterList'), scrollBar, scrollThumb, scrollTrack);
		
}	

function generalBodyCopyFunction(event)
{	
	event.clipboardData.setData('text/plain', CIPlugin.copyTemplateForFilter(currentFilterSelection.getAttribute("tag")));
	event.preventDefault();
	event.stopPropagation();
}
 
 // The event handler for the image drop.  This handles fetching the image URL and trying
// to place it inside of the widget.

function dragdrop (id, event)
{
	var uri = null;
	try {
	    uri = event.dataTransfer.getData("text/uri-list");	// attempt to load the new
	} catch (ex)											// image
	{
	}
		
	// if the acquisition is successful:
	if (uri)
	{
		var img;
		img = document.getElementById (id);				// then, assign the new
		img.src = uri;								// and the "hiddenPic"; a
		//img.onload = preResize;									// load handler is called
		
		// setup the filter preview
		var plugin = document.embeds["CIPluginPreview"];
		plugin.setImageByPath(uri, id);
		imagePreviewArray[id] = uri;
	}															// when the image has loaded

	event.stopPropagation();
	event.preventDefault();
}


// The dragenter, dragover, and dragleave functions are implemented but not used.  They
// can be used if you want to change the image when it enters the widget.

function dragenter (event)
{
	event.stopPropagation();
	event.preventDefault();
}

function dragover (event)
{
	event.stopPropagation();
	event.preventDefault();
}

function dragleave (event)
{
	event.stopPropagation();
	event.preventDefault();
}

// these functions are called when the info button itself receives onmouseover and onmouseout events

function enterPreview(event)
{
	document.getElementById('fitlerPreviewTitle').setAttribute("class", 'elementTitleHighlight');
}

function exitPreview(event)
{
	document.getElementById('fitlerPreviewTitle').setAttribute("class", 'elementTitle');
}




/*********************************/
// HIDING AND SHOWING PREFERENCES
/*********************************/

// showPrefs() is called when the preferences flipper is clicked upon.  It freezes the front of the widget,
// hides the front div, unhides the back div, and then flips the widget over.


function showPrefs()
{
	var front = document.getElementById("front");
	var back = document.getElementById("back");
	
	if (window.widget)
		widget.prepareForTransition("ToBack");		// freezes the widget so that you can change it without the user noticing
	
	front.style.display="none";		// hide the front
	back.style.display="block";		// show the back
	
	if (window.widget)
		setTimeout ('widget.performTransition();', 0);		// and flip the widget over	

	document.getElementById('fliprollie').style.display = 'none';  // clean up the front side - hide the circle behind the info button

}


// hidePrefs() is called by the done button on the back side of the widget.  It performs the opposite transition
// as showPrefs() does.

function hidePrefs()
{
	var front = document.getElementById("front");
	var back = document.getElementById("back");
	
	if (window.widget)
		widget.prepareForTransition("ToFront");		// freezes the widget and prepares it for the flip back to the front
	
	back.style.display="none";			// hide the back
	front.style.display="block";		// show the front
	
	if (window.widget)
		setTimeout ('widget.performTransition();', 0);		// and flip the widget back to the front
}


// PREFERENCE BUTTON ANIMATION (- the pref flipper fade in/out)

var flipShown = false;		// a flag used to signify if the flipper is currently shown or not.


// A structure that holds information that is needed for the animation to run.
var animation = {duration:0, starttime:0, to:1.0, now:0.0, from:0.0, firstElement:null, timer:null};


// mousemove() is the event handle assigned to the onmousemove property on the front div of the widget. 
// It is triggered whenever a mouse is moved within the bounds of your widget.  It prepares the
// preference flipper fade and then calls animate() to performs the animation.

function mousemove (event)
{
	if (!flipShown)			// if the preferences flipper is not already showing...
	{
		if (animation.timer != null)			// reset the animation timer value, in case a value was left behind
		{
			clearInterval (animation.timer);
			animation.timer  = null;
		}
		
		var starttime = (new Date).getTime() - 13; 		// set it back one frame
		
		animation.duration = 500;												// animation time, in ms
		animation.starttime = starttime;										// specify the start time
		animation.firstElement = document.getElementById ('flip');		// specify the element to fade
		animation.timer = setInterval ("animate();", 13);						// set the animation function
		animation.from = animation.now;											// beginning opacity (not ness. 0)
		animation.to = 1.0;														// final opacity
		animate();																// begin animation
		flipShown = true;														// mark the flipper as animated
	}
}

// mouseexit() is the opposite of mousemove() in that it preps the preferences flipper
// to disappear.  It adds the appropriate values to the animation data structure and sets the animation in motion.

function mouseexit (event)
{
	if (flipShown)
	{
		// fade in the flip widget
		if (animation.timer != null)
		{
			clearInterval (animation.timer);
			animation.timer  = null;
		}
		
		var starttime = (new Date).getTime() - 13;
		
		animation.duration = 500;
		animation.starttime = starttime;
		animation.firstElement = document.getElementById ('flip');
		animation.timer = setInterval ("animate();", 13);
		animation.from = animation.now;
		animation.to = 0.0;
		animate();
		flipShown = false;
	}
}


// animate() performs the fade animation for the preferences flipper. It uses the opacity CSS property to simulate a fade.

function animate()
{
	var T;
	var ease;
	var time = (new Date).getTime();
		
	
	T = limit_3(time-animation.starttime, 0, animation.duration);
	
	if (T >= animation.duration)
	{
		clearInterval (animation.timer);
		animation.timer = null;
		animation.now = animation.to;
	}
	else
	{
		ease = 0.5 - (0.5 * Math.cos(Math.PI * T / animation.duration));
		animation.now = computeNextFloat (animation.from, animation.to, ease);
	}
	
	animation.firstElement.style.opacity = animation.now;
}


// these functions are utilities used by animate()

function limit_3 (a, b, c)
{
    return a < b ? b : (a > c ? c : a);
}

function computeNextFloat (from, to, ease)
{
    return from + (to - from) * ease;
}

// these functions are called when the info button itself receives onmouseover and onmouseout events

function enterflip(event)
{
	document.getElementById('fliprollie').style.display = 'block';
}

function exitflip(event)
{
	document.getElementById('fliprollie').style.display = 'none';
}


/*

Copyright _ 2005, Apple Computer, Inc.  All rights reserved.
NOTE:  Use of this source code is subject to the terms of the Software
License Agreement for Mac OS X, which accompanies the code.  Your use
of this source code signifies your agreement to such license terms and
conditions.  Except as expressly granted in the Software License Agreement
for Mac OS X, no other copyright, patent, or other intellectual property
license or right is granted, either expressly or by implication, by Apple.

*/
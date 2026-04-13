/*
Copyright (c) 2005, Apple Computer, Inc.  All rights reserved.
NOTE:  Use of this source code is subject to the terms of the Software
License Agreement for Mac OS X, which accompanies the code.  Your use
of this source code signifies your agreement to such license terms and
conditions.  Except as expressly granted in the Software License Agreement
for Mac OS X, no other copyright, patent, or other intellectual property
license or right is granted, either expressly or by implication, by Apple.
*/

var gTestMode = false;

var gDebug = 0;

var gAirlines = new Array();
var GAirports = new Array();

var searchResults = new Array();
var currentSelection = null;
var currentSelectionIdx = -1;

var defaultAirlineCode = null;
var lastSelectedAirlineCode = null;
var defaultDepartCity = null;
var defaultArriveCity = null;
var defaultDepartureDate = null;
var defaultFlightNo = "";
var trackingFlightNo = "";
var trackingAirlineCode = "";
var gCurrentArriveCity = null;
var gCurrentDepartCity = null;
var gHighlightFlightNumberWhenListLoads = null;

var gDepartLocs = null;
var gArriveLocs = null;
var gOverallFlightStatus = null;

var gTrackingFlight = null;

var gDepartCombo = null;
var gArriveCombo = null;

var gCurrentHRange = null;
var gCurrentVRange = null;
var gCurrentXOffset = 0.0;

var gCachedTrackingData = null; // last cached and parsed data from XML feed from last "track"

var kStartMode = 0;
var kFindMode = 1;
var kTrackMode = 2;

var applicationMode = kStartMode;

var searchAnimateInterval = null;
var timerInterval = null;
var zoomInterval = null;

var gDocumentHeight = 0;
var gViewHeight = 0;
var scrollBarHeight = 156;

var kCanvasWidth = 237.0;
var kCanvasHeight = 172.0;
var kMapWidth = 2272.0;
var kMapHeight = 1526.0;
var kHiResWidthRatio = kMapWidth/kCanvasWidth;
var kHiResHeightRatio = kMapHeight/kCanvasHeight;

var kPixelsPerRadian = kMapWidth/(2*Math.PI);
var kPrimeMeridian = 1067.0;	// 1069 1072
var kEquatorHeight = 1044.5;	// 1042 1029

// Load images
var mapImg = new Image(kMapWidth, kMapHeight);
mapImg.src = "Images/mercatormap_gray.png";   
mapImg.onerror = imageLoadError;
mapImg.onload  = imageLoadComplete;

var colorMapImg = new Image(kMapWidth, kMapHeight);
colorMapImg.src = "Images/mercatormap_color.png";   
colorMapImg.onerror = imageLoadError;
colorMapImg.onload  = imageLoadComplete;

var planeImg = new Image(22,19);
planeImg.src = "Images/plane.png";

var planeShadowImg = new Image(22,19);
planeShadowImg.src = "Images/plane_shadow.png";

var gZoomRect = {x:0.0, y:0.0, width:kMapWidth, height:kMapHeight};
var gZoomSrcRect = {x:0.0, y:0.0, width:kMapWidth, height:kMapHeight};

var lastXMLRequest = null;
var searchFirstTime = true;
var trackFirstTime = true;


function printBackTrace()
{
	var fn = arguments.caller;
	alert("backTrace");
	while (fn != null) {
		alert("    "+fn.toString());
		fn = fn.caller;
	}
}

function imageLoadError()
{
    alert("ERROR loading image!!!!");
}

function imageLoadComplete()
{
//    alert("COMPLETED loading image!!!!");
}

function getLocalizedString (key) {
	try {
		var value = localizedStrings[key];
		if (value == null)
			value = key;
		return value;
	} catch (ex) {if(gDebug > 0) alert(key);}

	return key;
}

function domToString(dom)
{
	return "{id:"+dom.id+", class:"+dom.className+", nodeName:"+dom.nodeName+"}";
}

function dumpToString(o,depth,newLines,indent)
{
	var retVal = "";
	if ((newLines == null) || (newLines == true))
		newLines = depth - 1;
	if (indent == null)
		indent = 1;
	if (depth == null)
		depth = 1;
	if ((typeof o == "string") || (typeof o == "number") || (typeof o == "boolean") || (typeof o == "function")) {
		if (newLines >= 0)
			for (ii = 0; ii < indent; ii++)	retVal += "    ";
		retVal += "'"+o+(newLines >= 0 ?"'\n" : "' ");
	} else if (depth > 0) {
		for (ii = 0; ii < indent; ii++)	retVal += "    ";
		retVal += (newLines > 0 ? "{\n" : "{");
		for (f in o) {
			if (newLines > 0)
				for (ii = 0; ii < indent; ii++)	retVal += "    ";
			retVal += f+":"+dumpToString(o[f],depth-1,newLines-1,indent+1);
		}
		for (ii = 0; ii < (indent-1); ii++)	retVal += "    ";
		retVal += (newLines >= 0 ? "}\n" : "}");
	} else if (newLines >= 0)
		retVal += "\n";
	return retVal;
}


function canDoAction()
{
	var		buttonText = document.getElementById('actionButton-label').innerText;
	if ((applicationMode == kFindMode) && (buttonText == getLocalizedString('Track Flight')) ) {
		return (currentSelectionIdx >= 0);
	} else {
		if (	(defaultDepartCity != null) 
				&& 	(defaultArriveCity != null)
				&&  (defaultDepartCity.length > 0) 
				&&  (defaultArriveCity.length > 0)
				&& 	(	(defaultArriveCity.length > 0) || ((defaultAirlineCode != null) && defaultFlightNo != null))
			   ) {
//		alert("canDoAction:"+(defaultDepartCity ? defaultDepartCity : "null")
//			+"-->"+(defaultArriveCity ? defaultArriveCity : "null")
//			+"("+(defaultAirlineCode ? defaultAirlineCode: "null")+","+defaultFlightNo+") ==> TRUE");
	
			return true;
		} else {
		// alert("canDoAction:"+(defaultDepartCity ? defaultDepartCity : "null")
		// 	+"-->"+(defaultArriveCity ? defaultArriveCity : "null")
		// 	+"("+(defaultAirlineCode ? defaultAirlineCode: "null")+","+defaultFlightNo+") ==> FALSE");
	
			return false;
		}
	}
}

function updateActionButtonEnabled() {
    var btnEnabled = canDoAction();
    var actionButton = document.getElementById('actionButton-label');
    var btnSuffix = (btnEnabled ? "_active" : "");
    
    document.getElementById('action-buttonLeft').src = "Images/button_left" + btnSuffix + ".png"; 
    document.getElementById('action-buttonRight').src = "Images/button_right" + btnSuffix + ".png";
                   
    actionButton.style["background"] = "url(Images/button_center" + btnSuffix + ".png) repeat top left";
    if (btnEnabled) {
		actionButton.style["color"] = "white";
	} else {
		actionButton.style["color"] = "#848484";
	}
}

function setupWidget() {
    if (window.widget)
    {
         document.getElementById('airline-label').innerText = getLocalizedString('Airline:');
         document.getElementById('depart-label').innerText = getLocalizedString('Depart City:');
         document.getElementById('arrive-label').innerText = getLocalizedString('Arrive City:');
         document.getElementById('actionButton-label').innerText = getLocalizedString('Find Flights');
         document.getElementById('status-label').innerText = getLocalizedString('StartInstructions');
         
         document.getElementById('airlineHeader').innerText = getLocalizedString('Airline');
         document.getElementById('flightNoHeader').innerText = getLocalizedString('FlightShort');
         document.getElementById('statusHeader').innerText = getLocalizedString('Status');
         
         //document.getElementById('canvas').getContext("2d").globalCompositeOperation = "source-atop";
    }

	//Make info button
	new AppleInfoButton(document.getElementById('info'),document.getElementById('front'), "white", "white", showbackside);

    // Create backside button
   	var doneButton = document.getElementById('done-button');
	new AppleGlassButton (doneButton, getLocalizedString('Done'), donePressed);
//    doneButton.childNodes[1].style.minWidth = '38px';
    if (window.widget)
	{
	   	if ((widget.preferenceForKey('FTTestMode') != null) && (parseInt(widget.preferenceForKey('FTTestMode'))  != 0))
	  	{
	  		gTestMode = true;	// to indicate that we're in a special testing mode.  May show invalid data so don't use for production
	  	}
	}
    
    gDepartCombo = document.embeds['departCity-input'];
    gArriveCombo = document.embeds['arriveCity-input'];
	gAirlineCombo = document.embeds['airlines-input'];

    gAirlineCombo.setNextKeyView(gDepartCombo);
    gDepartCombo.setNextKeyView(gArriveCombo);
    gArriveCombo.setNextKeyView(gAirlineCombo);
    
    // gAirlineCombo.setOnchange(airlineTextChanged);
    // gDepartCombo.setOnchange(departCityTextChanged);
    // gArriveCombo.setOnchange(function(){alert("setOnChange"); arriveCityTextChanged()});

    gAirlineCombo.setOnkeypress(airlineTextChanged);
    gDepartCombo.setOnkeypress(departCityTextChanged);
    gArriveCombo.setOnkeypress(arriveCityTextChanged);
    
    gAirlineCombo.setOnTargetActionTriggered(airlineComboTargetActionTriggered);
    gDepartCombo.setOnTargetActionTriggered(departComboTargetActionTriggered);
    gArriveCombo.setOnTargetActionTriggered(arriveComboTargetActionTriggered);

    populateAirlineSelect();
    populateAirportSelect();
    setTimeout('drawMap();',0);

	loadPreferences();

	if(gTrackingFlight)
		trackFlight(gTrackingFlight);
	else
	{
		updateCombosWithDefaults();
   		updateUIWithMode(kStartMode);		
	}

	gAirlineCombo.selectText();
}

function loadPreferences()
{
	if(window.widget)
	{
		var validStartuprequest;
		
		//first check for a startup request
		if(widget.startupRequest)
			validStartuprequest = setGlobalsFromRequest(widget.startupRequest);

		//widget preferences
		if(!validStartuprequest) //if we didnt find a airline code and flight number
		{
			trackingAirlineCode = widget.preferenceForKey(createkey("airlineCode"));
			trackingFlightNo = widget.preferenceForKey(createkey("flightNumber"));
			gCurrentDepartCity = widget.preferenceForKey(createkey("departCity"));
			gCurrentArriveCity = widget.preferenceForKey(createkey("arriveCity"));
		}

		//if we have the minimum, make a flight to track
		if(trackingAirlineCode && trackingFlightNo)
		{
			defaultDepartCity = gCurrentDepartCity;
			defaultArriveCity = gCurrentArriveCity;
			defaultAirlineCode = trackingAirlineCode;		
			
			gTrackingFlight = {	
				airlineCode: trackingAirlineCode,
				flightNumber: trackingFlightNo,
				departCity: gCurrentDepartCity,
				arriveCity: gCurrentArriveCity,
			}
		}
		else
		{
			//general defaults
			defaultDepartCity = widget.preferenceForKey("defaultDepartCity");
			defaultArriveCity = widget.preferenceForKey("defaultArriveCity");
			defaultAirlineCode = widget.preferenceForKey("defaultAirlineCode");		
		}
	}
}

function updateUIWithMode(newMode)
{
    document.getElementById('detailArea').style.display = (newMode==kStartMode||newMode==kTrackMode)?"none":"block";
    document.getElementById('status-label').style.display = (newMode==kStartMode)?"block":"none";
    document.getElementById('backArrow-button').style.display = (newMode==kStartMode)?"none":"block";
    document.getElementById('action-button').style.display = (newMode==kTrackMode)?"none":"block";
    document.getElementById('flip').style.display = (newMode!=kStartMode)?"none":"block";
    
    gZoomRect = {x:0, y:0, width:kMapWidth, height:kMapHeight};

   	var oldCode = gAirlineCombo.selectedCode();
    if ( newMode == kStartMode ) {
		cancelAllTimers(); //dont ant to update if we go back to start
        setControlsEnabled(true);
		drawMap();
        if ( (defaultAirlineCode != null) && (defaultAirlineCode.length > 0) && (defaultAirlineCode != oldCode)) {
        	var airlineInfo = airlineForCode(defaultAirlineCode);
            gAirlineCombo.setValue(airlineInfo.name);
        }
        document.getElementById('status-label').innerText = getLocalizedString("StartInstructions");
        document.getElementById('actionButton-label').innerText = getLocalizedString('Find Flights');
    }
    else if ( newMode == kFindMode ) {
        setControlsEnabled(true);
        clearMap();
        
   			//        	if (lastSelectedAirlineCode == null) {
   			//        		gAirlineCombo.setValue("All Airlines");
   			// gAirlineCombo.setSelectedIndex(0);
   			// 	        defaultAirlineCode = null;
   			//        	}
        document.getElementById('actionButton-label').innerText = getLocalizedString('Track Flight');
        
        if ((defaultAirlineCode != null)  && (defaultAirlineCode.length > 0) && (defaultAirlineCode != oldCode)) {
        	var airlineInfo = airlineForCode(defaultAirlineCode);
        	if (airlineInfo != null) {
	            gAirlineCombo.setValue(airlineInfo.name);
	        }
        }
    }
    else if ( newMode == kTrackMode ) {
        setControlsEnabled(true);
        drawMap();
    }
    
    if ( newMode != kTrackMode ) {
    	document.getElementById('departTime-label').innerText = "";
        document.getElementById('arriveTime-label').innerText = "";
        document.getElementById('flightGate-label').innerText = "";
        document.getElementById('flightStatus-label').innerText = "";
        document.getElementById('airSpeed-label').innerText = "";
        document.getElementById('airAltitude-label').innerText = "";
        document.getElementById('airSpeedUnits-label').innerText = "";
        document.getElementById('airAltitudeUnits-label').innerText = "";
    }
    applicationMode = newMode;

    updateActionButtonEnabled();
}

function setControlsEnabled(enabled) 
{
    var color = enabled?"white":"gray"; // text color dependent on flag
    var dispValue = enabled?"block":"none"; // display value dependent also on 'enabled' flag
    
    document.getElementById('airline-label').style.color = color;
    document.getElementById('depart-label').style.color = color;
    document.getElementById('arrive-label').style.color = color;
    
    var itemIDs = ["airlines-","departCity-","arriveCity-"];
    for ( var i = 0; i < itemIDs.length; i++ ) 
    {
        var inputElem = document.getElementById(itemIDs[i]+"input");
        var wellElem = document.getElementById(itemIDs[i]+"well");
        var frameTPElem = document.getElementById(itemIDs[i]+"frametp");
        var frameBTElem = document.getElementById(itemIDs[i]+"framebt");
        var frameLTElem = document.getElementById(itemIDs[i]+"framelt");
        var frameRTElem = document.getElementById(itemIDs[i]+"framert");
        var popupArrowsElem = document.getElementById(itemIDs[i]+"popupArrows");
        
        if ( popupArrowsElem != null ) popupArrowsElem.style.display = dispValue;
        
        wellElem.style.display = dispValue;
        frameTPElem.style.display = dispValue;
        frameBTElem.style.display = dispValue;
        frameLTElem.style.display = dispValue;
        frameRTElem.style.display = dispValue;
        wellElem.style.display = dispValue;
    }
    
    gAirlineCombo.setEnabled(enabled);
    gDepartCombo.setEnabled(enabled);
    gArriveCombo.setEnabled(enabled);
    updateActionButtonEnabled();
}

var	gAnimateText;
var gPeriodCount;
var gAnimatePeriodMS = 1500;
var gRefreshTimeMS = 30;
var gRefreshsPerPeriod = gAnimatePeriodMS/gRefreshTimeMS;

function startServerAccessAnimation()
{
	gAnimateText = document.getElementById('status-label').innerText;
	stopServerAccessAnimation();
	gPeriodCount = gRefreshsPerPeriod;
	searchAnimateInterval = setInterval(animateServerAccess,gRefreshTimeMS);
}

function stopServerAccessAnimation()
{
	if (searchAnimateInterval != null) {
		clearInterval(searchAnimateInterval);
		searchAnimateInterval = null;
	}
	if (applicationMode != kFindMode)
		drawMap(false);
}

function animateServerAccess()
{
    drawMap(false);
    var value = Math.cos((Math.PI/2.0) * Math.abs(gPeriodCount-gRefreshsPerPeriod)/gRefreshsPerPeriod);

    var alphaNow = value * value;

    var canvas = document.getElementById("canvas");
    var context = canvas.getContext("2d");
    context.save();
    context.translate(116,125);
	context.globalAlpha = 0.45+0.55*alphaNow;
    context.drawImage(planeImg, -21/2, -9, 21, 18);
    context.restore();
	gPeriodCount = (gPeriodCount+2)%(2*gRefreshsPerPeriod);
}


function fetchFlightData()
{
    if ( lastXMLRequest != null )
        lastXMLRequest.abort();
   
    updateUIWithMode(kStartMode);
    if (gDebug > 0) 
    	alert("searchFlights");

    document.getElementById('status-label').style.display = "block";
    document.getElementById('status-label').innerText = getLocalizedString("Searching for Flights");
    document.getElementById('detailArea').style.display = "none";
    drawMap();
    
    defaultDepartCity = gDepartCombo.selectedCode();
    defaultArriveCity = gArriveCombo.selectedCode();
    defaultAirlineCode = gAirlineCombo.selectedCode();
	defaultDepartureDate = todayDateString();

	updateDepartPrefs (gDepartCombo.selectedCode());
	updateArrivePrefs (gArriveCombo.selectedCode());	
	
   	// if (lastSelectedAirlineCode == null) {
   	// 	gAirlineCombo.setSelectedIndex(0);
   	// }
	startServerAccessAnimation();
	
	clearCurrentSelection();
    setupUpdates(true);
}

var alreadyZoomed = false;

function setupUpdates(animate)
{

	if (timerInterval != null) {
		clearInterval(timerInterval);
		timerInterval = null;
	}

	if (animate == true)
		startServerAccessAnimation();

	if(applicationMode == kTrackMode) {
		detail = "true";
		callback = "trackingDataLoaded";
		code = "trackingAirlineCode";
	}  else {
		detail = "false";
		callback = "searchResultsLoaded";
		code = "defaultAirlineCode"
	}
	
	code = (code == null || code.length == 0) ? null : code;

	var jsFunc = "lastXMLRequest = performXMLRequest(" + code + ", trackingFlightNo, defaultDepartCity, defaultArriveCity, " + detail + ", " + callback + ", " + defaultDepartureDate  + ")";

	eval(jsFunc);
	timerInterval = setInterval(jsFunc, 5*60000); // every 5 minutes (only when its left open)
}

//Pulling this html parsing out into its own function is a step in a
//longer term refactoring 
function trackFlightWithResultIndex (resultIndex) {
	var row = searchResults[resultIndex]
	var flight = {};

	flight["airlineCode"] = row[0].children[0].getAttribute("code");
	flight["departCity"] = row[0].children[0].getAttribute("departcity");
	flight["arriveCity"] = row[0].children[0].getAttribute("arrivecity");
	flight["flightNumber"] = row[0].children[1].innerHTML;
	flight["statusString"] = row[0].children[2].innerHTML;
	trackFlight(flight);
}

function trackFlight (flightToTrack)
{
	if(!flightToTrack["airlineCode"] || !flightToTrack["flightNumber"])
		return;

    if ( lastXMLRequest != null )
        lastXMLRequest.abort();

	updateUIWithMode(kTrackMode);

	//start loading message
	document.getElementById('status-label').style.display = "block";
	document.getElementById('status-label').innerText = getLocalizedString("Getting flight information");

	//set globals
	//TODO: stop using these globals and just use gTrackingFlight
	trackingFlightNo = flightToTrack["flightNumber"];
	trackingAirlineCode = flightToTrack["airlineCode"];
	defaultDepartCity = flightToTrack["departCity"];
	defaultArriveCity = flightToTrack["arriveCity"];

	updateCombosForFlight(flightToTrack, true);
	savePreferencesForFlight(flightToTrack);

	defaultDepartureDate = todayDateString();

	var flightGateLabel = document.getElementById('flightGate-label');
	flightGateLabel.innerText = getLocalizedString("FlightShort") + " " + trackingFlightNo + " "
	shrinkToFit(flightGateLabel, 145, 11);

	resetZoom();
	gCurrentHRange = null;
	gCurrentVRange = null;
	gCurrentXOffset = 0.0;

	setupUpdates(true);
}

function setGlobalsFromRequest(request)
{
	var successful = false;
	
	if(request)
	{
		var tempAirlineCode = request["airlinecode"];
		var tempAirlineName = request["airlinename"];
		var tempFlightNo = request["flightnumber"];

		if(tempAirlineCode)
			tempAirlineCode = tempAirlineCode.toUpperCase();

		//if it's not a valid code, see if it is an airline name we know
		if(tempAirlineName && gAirlineCombo.dataForAirlineCode(trackingAirlineCode))
			tempAirlineCode = gAirlineCombo.codeForAirlineName(tempAirlineName);

		if(tempAirlineCode && tempFlightNo)
		{
			trackingAirlineCode = tempAirlineCode;
			trackingFlightNo = tempFlightNo;
			gCurrentDepartCity = request["departcity"];
			gCurrentArriveCity = request["arrivecity"];
			successful = true;
		}
	}
	
	return successful;
}

function savePreferencesFromCombos () {
	var fakeFlight = {
		airlineCode: gAirlineCombo.selectedCode(),
		departCity: gDepartCombo.selectedCode(),
		arriveCity: gArriveCombo.selectedCode()
	};
	savePreferencesForFlight(fakeFlight);
}

function savePrefsFromDefaults () {
	var defaultFlight = { //make a fake flight
			airlineCode: defaultAirlineCode, 
			flightNumber: trackingFlightNo,
			departCity: defaultDepartCity, 
			arriveCity: defaultArriveCity,
		}
	
	savePreferencesForFlight(defaultFlight);
}

function savePreferencesForFlight (flight) {

	if(flight["airlineCode"] == undefined)
		flight["airlineCode"] = null;

	if(flight["flightNumber"] == undefined)
		flight["flightNumber"] = null;

	if(flight["arriveCity"] == undefined)
		flight["arriveCity"] = null;

	if(flight["departCity"] == undefined)
		flight["departCity"] = null;
			
	widget.setPreferenceForKey(flight["airlineCode"], "defaultAirlineCode");
	widget.setPreferenceForKey(flight["airlineCode"], createkey("airlineCode"));			
	widget.setPreferenceForKey(flight["flightNumber"], createkey("flightNumber"));		

	widget.setPreferenceForKey(flight["arriveCity"], createkey("arriveCity"));		
	widget.setPreferenceForKey(flight["departCity"], createkey("departCity"));
}

function clearTrackingFlightNumber () {
	trackingFlightNo = null; //forget current flight number
	widget.setPreferenceForKey(null, createkey("flightNumber"));
}

function updateDepartPrefs (airportCode) {
	widget.setPreferenceForKey (airportCode, "defaultDepartCity");
	widget.setPreferenceForKey (airportCode, createkey("departCity"));	
}

function updateArrivePrefs (airportCode) {
	widget.setPreferenceForKey (airportCode, "defaultArriveCity");
	widget.setPreferenceForKey (airportCode, createkey("arriveCity"));
}

function updateCombosWithDefaults () {
	var defaultFlight = { //make a fake flight
			airlineCode: defaultAirlineCode, 
			flightNumber: null,
			departCity: defaultDepartCity, 
			arriveCity: defaultArriveCity,
		}
	
	updateCombosForFlight(defaultFlight);
}

function updateCombosForFlight (flight) {

	if (flight["airlineCode"]) {
		gAirlineCombo.setValue(airlineForCode(flight["airlineCode"]).name);
		gAirlineCombo.setSelectedCode(flight["airlineCode"]);
	}

	if(flight["departCity"]) {
    	gDepartCombo.setSelectedCode(flight["departCity"]);
    }

    if (flight["arriveCity"]) {
  		gArriveCombo.setSelectedCode(flight["arriveCity"]);
    }
}

function handleXMLError(errorCode) 
{
    // Handles error codes returned by XML feed
    if ( errorCode != 0 ) 
    {
    	drawMap(false);
//		alert('handleXMLError: errorCode = '+errorCode);
        var statusLabel = document.getElementById('status-label');
        statusLabel.style.display = "block";
        
        // Handle errors returned by server
        switch (errorCode) {
        	case -2: statusLabel.innerText = getLocalizedString('NoFlightInfoAvailable');		break;
        	case -1: statusLabel.innerText = getLocalizedString('ErrorNoServer');				break;
			case  1: statusLabel.innerText = getLocalizedString('ErrorSystemUnavailable');		break;
			case  2: statusLabel.innerText = getLocalizedString('NoFlightInfoAvailable');		break;
			case  3: statusLabel.innerText = (applicationMode==kTrackMode
											?	getLocalizedString('NoFlightInfoAvailable')
											:	getLocalizedString('ErrorNothingFound')
											);
	        	break;
	        case  4: statusLabel.innerText = getLocalizedString('ErrorInvalidEntity');			break;
	        case  5: statusLabel.innerText = getLocalizedString('ErrorInvalidSearchParams');	break;
	      }
        if ( statusLabel.innerText.length > 0 )
            return true;
    }
    
    return false;
}

function formatMinutesTime(minutes) {
    if ( minutes / 60 >= 1 )
        return parseInt((minutes/60).toString())+" "
              +getLocalizedString("Hr").toLowerCase()+" "+(minutes%60)+" "
              +getLocalizedString("Mins").toLowerCase();
    else
        return minutes+" "+getLocalizedString("Mins").toLowerCase();
}

function getAirportLocation(airportList, airportCode,prevLoc) {
	var airportServerData = airportList[airportCode];
	var location = null;
	if (airportServerData != null) {
		location = locationFromStrings(airportServerData.latitude,airportServerData.longitude,prevLoc);
	} else {
		alert("getAirportLocation - ERROR - server failed to return airport info for airport code: "+airportCode);
		return null;
	}
    return location;
}

function degreesToRadians(degrees) {
    return (degrees/360.0) * 2*Math.PI;
}

function radiansToDegrees(radians) {
    return (radians/(2*Math.PI)) * 360.0;
}

function dotProduct(v1,v2) {
    return (v1.x * v2.x) + (v1.y * v2.y) + (v1.z * v2.z);
}

function thetaToX(theta,prevLoc) {
	
    var thetaRadians = degreesToRadians(theta);
    var x = kPrimeMeridian + kPixelsPerRadian*thetaRadians;
    if (prevLoc != null) {
		if (Math.abs(prevLoc.x - x) > (kMapWidth/2)) {
			if (x < prevLoc.x) {
				x += kMapWidth;
			} else {
				x -= kMapWidth;
			}
		}
	}
    return x;
}

function psiToY(psi) {
    psiRadians = degreesToRadians(psi);
    var y = kEquatorHeight - kPixelsPerRadian * Math.log(Math.tan((Math.PI/4)+(psiRadians/2.0)) );
    return y;
}

function longitudeToTheta(longitudeStr) 
{
    var longitude = parseFloat(longitudeStr.substr(0,longitudeStr.length-1));
	
    if ( longitudeStr.substr(longitudeStr.length-1,1) == "W" ) // make sure "West" is always one sign
        longitude = -longitude;
    else                                                       // make sure "East" is always one sign
        longitude = longitude;    
    return longitude; 
}

function latitudeToPsi(latitudeStr) 
{
    var latitude = parseFloat(latitudeStr.substr(0,latitudeStr.length-1));

    if ( latitudeStr.substr(latitudeStr.length-1,1) == "S" ) // make sure "South" is always one sign
        latitude = -latitude;
    else                                                       // make sure "North" is always one sign
        latitude = latitude;
        
    return latitude;
}

// location contains both sperical coordinates (theta,phi) and mercator projected cartesian (x,y) in map scaled space

function locationFromThetaPsi(ttheta,ppsi,prevLoc)
{
	xx = thetaToX(ttheta,prevLoc);
	yy = psiToY(ppsi);
	return {x:xx, y:yy, theta:ttheta, psi:ppsi};
}

function locationFromStrings(latitudeStr,longitudeStr,prevLoc)
{
	theta = longitudeToTheta(longitudeStr);
	psi = latitudeToPsi(latitudeStr);
	return locationFromThetaPsi(theta,psi,prevLoc);
}


function transformToZoomCoord(x, y) {
    // Transform to zoom coord
    var transformCoord = {x: (x-gZoomRect.x)*(kCanvasWidth/gZoomRect.width), 
                          y: 5 + (y-gZoomRect.y)*(kCanvasHeight/gZoomRect.height)};

     // Now transform to canvas coord
//    transformCoord.x = (transformCoord.x / kMapWidth) * kCanvasWidth;
//    transformCoord.y = ((transformCoord.y / kMapHeight) * kCanvasHeight);    
    return transformCoord;
}

function clearMap() {
    var canvas = document.getElementById("canvas");
	var context = canvas.getContext("2d");
	
	context.clearRect (0, 0, kCanvasWidth, kCanvasHeight);
}

var gCornerRadius = 10;

function setMapImageClip(context)
{
	context.beginPath();
    context.moveTo(2,4);
    context.lineTo(kCanvasWidth-4-gCornerRadius*2,4);
    context.arcTo(kCanvasWidth-3,4,kCanvasWidth-2,4+gCornerRadius*2,gCornerRadius+2);
    context.lineTo(kCanvasWidth-3,kCanvasHeight-gCornerRadius*2);
    context.arcTo(kCanvasWidth-3,kCanvasHeight,kCanvasWidth-2-gCornerRadius*2,kCanvasHeight,gCornerRadius);
    context.lineTo(2,kCanvasHeight);
    context.lineTo(2,4);
    context.clip();
}

function drawMap(color) {
    var canvas = document.getElementById("canvas");
	var context = canvas.getContext("2d");
	
	canvas.style.display = "block";
	canvas.style.opacity = "1.0";
	context.clearRect (0, 0, kCanvasWidth, kCanvasHeight);
	
	context.save();
	
 	setMapImageClip(context);
 	if ((color == null) || (color == false)) {
	    context.drawImageFromRect(mapImg,0,0,kCanvasWidth,kCanvasHeight,0,5,kCanvasWidth,kCanvasHeight,"source-over");
  	} else if ((gZoomRect.x >= 0) && ((gZoomRect.x + gZoomRect.width) <= kMapWidth)) {
	    context.drawImageFromRect(colorMapImg,gZoomRect.x,gZoomRect.y,gZoomRect.width,gZoomRect.height,0,5,kCanvasWidth,kCanvasHeight, "source-over");
    } else if (gZoomRect.x < 0) {
    	var fullWidth = gZoomRect.width;
    	var x1 = 0;
    	var width1 = gZoomRect.width+gZoomRect.x;
    	var y = gZoomRect.y;
    	var height = gZoomRect.height;
    	var canvasWidth1 = kCanvasWidth*(width1/fullWidth);
    	var x2 = kMapWidth+gZoomRect.x;
    	var width2 = -gZoomRect.x;
    	var canvasWidth2 = kCanvasWidth*(width2/fullWidth);
	    context.drawImageFromRect(colorMapImg,x1, y, width1, height, canvasWidth2, 5, canvasWidth1, kCanvasHeight, "source-over");
	    context.drawImageFromRect(colorMapImg,x2, y, width2, height, 0,5, canvasWidth2, kCanvasHeight, "source-over");
    } else {
    	var fullWidth = gZoomRect.width;
    	var x1 = gZoomRect.x;
    	var width1 = kMapWidth-gZoomRect.x;
    	var y = gZoomRect.y;
    	var height = gZoomRect.height;
    	var canvasWidth1 = kCanvasWidth*(width1/fullWidth);
    	var x2 = 0;
    	var width2 = gZoomRect.x+fullWidth-kMapWidth;
    	var canvasWidth2 = kCanvasWidth*(width2/fullWidth);
	    context.drawImageFromRect(colorMapImg,x1, y, width1, height, 0, 5, canvasWidth1, kCanvasHeight, "source-over");
	    context.drawImageFromRect(colorMapImg,x2, y, width2, height, canvasWidth1, 5, canvasWidth2, kCanvasHeight, "source-over");
    }
	context.restore();
}

function calcCoordOnGeodesicCurve(t, departCoord, arriveOrthoCoord,prevLoc) {
    // Compute this point on the parametric path in cartesian coordinates
    var result = {x: Math.cos(t)*departCoord.x + Math.sin(t)*arriveOrthoCoord.x,
                  y: Math.cos(t)*departCoord.y + Math.sin(t)*arriveOrthoCoord.y,
                  z: Math.cos(t)*departCoord.z + Math.sin(t)*arriveOrthoCoord.z};
    
    // Convert cartesian coordinates back to spherical coordinates
    var resultThetaRadians = Math.atan2(result.y,result.x);
    var resultPsiRadians   = Math.atan2(Math.sqrt(result.x*result.x +result.y*result.y),result.z);
    
    // Convert back to longitude/latitude coordinates
    resultTheta = radiansToDegrees(resultThetaRadians);
    resultPsi  = 90 - radiansToDegrees(resultPsiRadians);
    return locationFromThetaPsi(resultTheta,resultPsi,prevLoc);
}

function growHRangeToIncludeLocation(range,location)
{
	var xx = location.x;
	if (range == null) {
		range = {left:xx,right:xx+1};
	} else {
		if (xx < range.left) {
			range.left = xx;
		} else if (xx > range.right) {
			range.right = xx;
		}
	}
	return range;
}

function growVRangeToIncludeLocation(range,location)
{
	var yy = location.y;
	if (range == null) {
		range = {top:yy,bottom:yy+1};
	} else {
		if (yy < range.top) {
			range.top = yy;
		} else if (yy > range.bottom) {
			range.bottom = yy;
		}
	}
	return range;
}

function calcXShiftOffsetForMap()
{
	hRange = gCurrentHRange;
	if ((hRange.left >= 0) && (hRange.right <= kMapWidth))
		return 0.0;
	else {
		var rangeWidth = hRange.right - hRange.left;
//		if (hRange.left < 0) {
//			return (kMapWidth + hRange.left) - (kMapWidth/2 - rangeWidth/2);
//		} else {
			return hRange.left - (kMapWidth/2 - rangeWidth/2);
//		}
	}
}

function drawPlaneShadow(context,planePos,planeAngle,tracking,progressRatio )
{
    var altitudeShadowOffset = 0.0;
    
    // Draw the plane shadow relative to the actual altitude of the flight, max of 6 pixels
    if ( tracking.altitude != null ) altitudeShadowOffset = (tracking.altitude/38000) * 6.0;
    // If we don't have altitude info, approximate the shadow using a sin() curve that approximates good shadow
    else if ( progressRatio != null ) 
        altitudeShadowOffset = Math.sin(Math.PI * progressRatio) * 6.0; // up to 6 pixel offset
    
    if ( altitudeShadowOffset > 0 ) {
        context.save();
        context.translate(planePos.x-2, planePos.y + altitudeShadowOffset);
        context.rotate(planeAngle);
        context.drawImage(planeShadowImg, -21/2, -9, 21, 18);
        context.restore();
    }
}

function drawPlane(context,planePos,planeAngle)
{
    context.save();
    context.translate(planePos.x,planePos.y);
    context.rotate(planeAngle);
    context.drawImage(planeImg, -21/2, -9, 21, 18);
    context.restore();
}
function drawCurvedPaths(context, departLocs,arriveLocs, planeLoc, progressRatio) {
    var		ii;
    var		numPaths = departLocs.length;
    if (arriveLocs.length != numPaths) 
    	alert("arriveLocs - ERROR - inconsisent sizes for departLocs and arrivelocs\n"+dumpToString(departLocs,2,false)+"\n"+dumpToString(arriveLocs,2,false));
    var shortestDistanceToPlane = 999999999999999999;
    var shortestPoint = {x:-1,y:-1,angle:0};
    
    var prevLoc = null;
	context.save();
	context.setStrokeColor("#FFBA00",1.0);
	context.setLineWidth(2.0);
	context.setLineCap("round");
    for (ii = 0; ii < numPaths; ii++) {
    	var departLoc = departLocs[ii];
    	var arriveLoc = arriveLocs[ii];
    	var rSquared = 1; // kPixelsPerRadian*kPixelsPerRadian;
    	var departCoord = {x: 1 * Math.sin(degreesToRadians(90-departLoc.psi))*Math.cos(degreesToRadians(departLoc.theta)), 
						   y: 1 * Math.sin(degreesToRadians(90-departLoc.psi))*Math.sin(degreesToRadians(departLoc.theta)),
						   z: 1 * Math.cos(degreesToRadians(90-departLoc.psi))};
		var arriveCoord = {x: 1 * Math.sin(degreesToRadians(90-arriveLoc.psi))*Math.cos(degreesToRadians(arriveLoc.theta)), 
						   y: 1 * Math.sin(degreesToRadians(90-arriveLoc.psi))*Math.sin(degreesToRadians(arriveLoc.theta)),
						   z: 1 * Math.cos(degreesToRadians(90-arriveLoc.psi))};
		var w = {x: arriveCoord.x - ( (dotProduct(departCoord,arriveCoord)/1) * departCoord.x ),
				 y: arriveCoord.y - ( (dotProduct(departCoord,arriveCoord)/1) * departCoord.y ),
				 z: arriveCoord.z - ( (dotProduct(departCoord,arriveCoord)/1) * departCoord.z )};
			  
		var wMagnitude = Math.sqrt(Math.pow(w.x,2) + Math.pow(w.y,2) + Math.pow(w.z,2));
		var arriveOrthoCoord = {x: (1/wMagnitude) * w.x,
								y: (1/wMagnitude) * w.y,
								z: (1/wMagnitude) * w.z};
	
		var deltaTheta = arriveLoc.theta - departLoc.theta;
		var deltaPsi  = arriveLoc.psi - departLoc.psi;

		var departDrawPoint = transformToZoomCoord(departLoc.x, departLoc.y);
		prevLoc = departLoc;
    	gCurrentVRange = growVRangeToIncludeLocation(gCurrentVRange,departLoc);
    	context.beginPath();
		context.moveTo(departDrawPoint.x,departDrawPoint.y);
		
		var domain = Math.acos(dotProduct(departCoord,arriveCoord)/1);
		var step = (domain/20.0)*(gZoomRect.width/kMapWidth);
		var lastlong;
		var lastPoint = null;
		var dx;
		var dy;
        for ( var t = 0; t < domain; t = t + step*Math.cos(degreesToRadians(prevLoc.psi))+.001) {
			// Compute new point on parametric path
			geodesicPoint = calcCoordOnGeodesicCurve(t,departCoord, arriveOrthoCoord,prevLoc);
			if (planeLoc != null) {
				var dxPlane = geodesicPoint.x-planeLoc.x;
				var dyPlane = geodesicPoint.y-planeLoc.y;
				distanceToPlane = dxPlane*dxPlane + dyPlane*dyPlane;
				if ( distanceToPlane < shortestDistanceToPlane ) {
					shortestPoint.x = geodesicPoint.x;
					shortestPoint.y = geodesicPoint.y;
					if (prevLoc != null)
						shortestPoint.angle = Math.atan2( (geodesicPoint.y - prevLoc.y), (geodesicPoint.x - prevLoc.x) );
					shortestDistanceToPlane = distanceToPlane;
				}
			}
			prevLoc = geodesicPoint;
			gCurrentVRange = growVRangeToIncludeLocation(gCurrentVRange,geodesicPoint);
			
			var drawPoint = transformToZoomCoord(geodesicPoint.x,geodesicPoint.y);
			if (lastPoint == null)
				lastPoint = drawPoint;
			
			// Use a sinusoidal line width peaking at the center of the curve
			context.beginPath();
			context.setLineWidth( 2.0 * Math.sin(Math.PI * (t/domain)) + 0.5);
			if ( t != 0 /*&& !(Math.abs(result.x-lastlong) > 119)*/ ) {
				context.moveTo(lastPoint.x,lastPoint.y);
				context.lineTo(drawPoint.x,drawPoint.y);
				context.stroke();
			}
			else
				context.moveTo(drawPoint.x,drawPoint.y);
			lastX = geodesicPoint.x;
			lastPoint.x = drawPoint.x;
			lastPoint.y = drawPoint.y;
		}
		// Finish off the path
		context.moveTo(lastPoint.x,lastPoint.y);
		var arriveDrawPoint = transformToZoomCoord(arriveLoc.x, arriveLoc.y);
		prevLoc = arriveLoc;
    	gCurrentVRange = growVRangeToIncludeLocation(gCurrentVRange,arriveLoc);
//		context.lineTo(arriveDrawPoint.x,arriveDrawPoint.y); 
		context.stroke();

    }
	context.restore();
    topLeft = transformToZoomCoord(gCurrentHRange.left,gCurrentVRange.top);
    botRight = transformToZoomCoord(gCurrentHRange.right,gCurrentVRange.bottom);
//	context.strokeRect(topLeft.x,topLeft.y,botRight.x-topLeft.x,botRight.y-topLeft.y);
    return shortestPoint;
}

function isInNorthAmerica(loc)
{
// estimate whether the plane is in north america
	lon = loc.theta % 360;
	lat = loc.psi;
	if ((lon > -50) || (lon < -170))
		return false;
	else if (lat < 25)
		return false;
	return true;
}

function isPlaneLocationAllowed(flightStatusInfo,planePos)
{
	var retVal = true;
	if (gTestMode) { 	// for testing purposes only.  Data not valid for production use.
		flightStatusInfo.isPlaneLocationAllowed = true;
		return true;
	}
	var leg;
	if (flightStatusInfo.status == "Enroute") {
		leg = flightStatusInfo.leg;
		departAirport = airportForCode(leg.depart.airportCode);
		arriveAirport = airportForCode(leg.arrive.airportCode);
		if ((departAirport.country == "Mexico") || (arriveAirport.country == "Mexico"))
			retVal = false;
		else
			retVal = isInNorthAmerica(leg.arriveLoc) && isInNorthAmerica(leg.departLoc);
	}
	flightStatusInfo.isPlaneLocationAllowed = retVal;
	return retVal;
}

function drawFlightPath(departLocs,arriveLocs,flightStatusInfo) {

	var departLocs = gDepartLocs;
	var arriveLocs = gArriveLocs;
	var flightStatusInfo = gOverallFlightStatus;
		
    gCurrentXOffset = calcXShiftOffsetForMap();
    if (alreadyZoomed == false)
		gZoomRect.x = gCurrentXOffset;

	if (gDebug > 0) 
		for (var ii = 0; ii < departLocs.length; ii++) {
			alert("leg["+ii+"]: "+dumpToString(departLocs[ii],1,false)+" --> "+dumpToString(arriveLocs[ii],1,false));
		}
	var tracking = flightStatusInfo.leg.tracking;
	var progressRatio = flightStatusInfo.progressRatio;
	
    var canvas = document.getElementById("canvas");
    var context = canvas.getContext("2d");
	
    canvas.style.opacity = "1.0";
    document.getElementById('detailArea').style.display = "none";

    var planeLoc;
	if ( (tracking.longitude != null) && (tracking.latitude != null ) )
	{
		var prevLoc = flightStatusInfo.leg.departLoc; 
		if (gDebug > 0) alert("tracking spherical .long/.lat: " + tracking.longitude + ", " + tracking.latitude);
		// ??? there appears to be a bug in the servers tracking info to make it return the wrong sign for tracking west longitudes.  You get -138W where it should be 138W
		theta = longitudeToTheta(tracking.longitude);
		if ((tracking.longitude.substr(0,1) == "-") && (tracking.longitude.substr(tracking.longitude.length-1,1) == "W"))
			theta = -theta;
		psi = latitudeToPsi(tracking.latitude);
		planeLoc = locationFromThetaPsi(theta,psi,prevLoc);		
	}
    context.clearRect (0, 0, kCanvasWidth, kCanvasHeight); 
    context.save();
 	setMapImageClip(context);
    drawMap(true);
    
	// Draw flight path first
	var closestPoint = drawCurvedPaths(context,departLocs,arriveLocs,planeLoc,progressRatio);
    var planePos = {x:0, y:0};
    var arriveLoc = flightStatusInfo.leg.arriveLoc;
    var departLoc = flightStatusInfo.leg.departLoc;
	switch (flightStatusInfo.status) {
		case "Landed":
			planePos.x = flightStatusInfo.leg.arriveLoc.x;
			planePos.y = flightStatusInfo.leg.arriveLoc.y;
			break;
		case "Scheduled":
			planePos.x = flightStatusInfo.leg.departLoc.x;
			planePos.y = flightStatusInfo.leg.departLoc.y;
			break;
		case "Stopped":
			planePos.x = flightStatusInfo.leg.arriveLoc.x;
			planePos.y = flightStatusInfo.leg.arriveLoc.y;
			arriveLoc = flightStatusInfo.nextLeg.arriveLoc;
    		departLoc = flightStatusInfo.nextLeg.departLoc;
       	break;
       	default:
			if ( (closestPoint.x != -1) && (closestPoint.y != -1 )) {
				planePos.x = closestPoint.x;
				planePos.y = closestPoint.y;
			} else {
				// convert lat/long degrees to pixel coordinates 
				planePos.x = planeLoc.x;
				planePos.y = planeLoc.y;
			}
			if ( tracking.longitude == null || tracking.latitude == null )
				alert("no latitude/longitude info and we're not on the ground!");
			break;
    }
	if (isPlaneLocationAllowed(flightStatusInfo,planePos)) {
		if (gDebug > 0) alert("planePos before: "+planePos.x+","+planePos.y);
		planePos = transformToZoomCoord(planePos.x,planePos.y);
		if (gDebug > 0) alert("planePos after: "+planePos.x+","+planePos.y);
		
		// Calculate plane angle
		var planeAngle;
		if ( (closestPoint.x != -1) && (closestPoint.y != -1) )
		{
			planeAngle = closestPoint.angle;
		}
		else if ( tracking.heading == null )
		{
			planeAngle = Math.atan2( (arriveLoc.y-departLoc.y), 
									 (arriveLoc.x - departLoc.x) );
		}
		else 
		{
			planeAngle = -(2*Math.PI/360.0)*(90-tracking.heading);
		}
		// Draw plane shadow
		drawPlaneShadow( context, planePos, planeAngle, tracking, progressRatio );
	
		// Then draw the plane
		drawPlane(context, planePos, planeAngle);
	}	
    context.restore();
}


function trackingDataLoaded(data, errorString) 
{
	stopServerAccessAnimation();
    gCachedTrackingData = data;

	//handle errors
    if ( data == null || applicationMode != kTrackMode ) {
    	if (errorString != null)
	        alert("trackingDataLoaded: errorString = "+errorString);
        return;
    }
    if ( handleXMLError(data.error) ) return;

    document.getElementById('status-label').style.display = "none";
    
    var flightList = data.flights;
    var listCount = flightList.length;
    var departLocs = new Array();
    var arriveLocs = new Array();
    var departLoc;
    var arriveLoc;
    var prevLoc = null;
    var legCount = 0;
    var flight = flightList[0];
    var	firstLeg = null;
	var lastLeg = null;
	
	if(flightList.length > 1)
		alert("ERROR - Received data for more than one flight");
	else(flight.length)
	{
		var	firstSchedLeg = null;
		var	lastLandedLeg = null;
		var enrouteLeg = null;
		var overallFlightStatus = null;
		if (flight.number == trackingFlightNo) {
			var legs = flight.legs;
			legCount = legs.length;
			lastLeg = legs[legCount-1];
			for ( var j = 0; j < legCount; j++ ) {  // Loop through every leg of the flight
				var leg = legs[j];
				leg.depart.scheduledDate = dateForLegInfoWithAttribute(leg.depart,'scheduled');
				leg.depart.bestDate = dateForLegInfoWithAttribute(leg.depart,'best');
				leg.arrive.scheduledDate = dateForLegInfoWithAttribute(leg.arrive,'scheduled');
				leg.arrive.bestDate = dateForLegInfoWithAttribute(leg.arrive,'best');
				var flightStatus = leg.status;
				var departInfo = leg.depart;
				var arriveInfo = leg.arrive;
				leg.legNumber = j;
				if (firstLeg == null)
					firstLeg = leg;
				if (flightStatus == "Enroute")
					enrouteLeg = leg;
				else if (flightStatus == "Landed")
					lastLandedLeg = leg;
				else if ((flightStatus == "Scheduled") && (firstSchedLeg == null)) 
					firstSchedLeg = leg;
				departLoc = getAirportLocation(data.airports, departInfo.airportCode, prevLoc);
				prevLoc = departLoc;
				arriveLoc = getAirportLocation(data.airports, arriveInfo.airportCode, prevLoc);
				if ((departLoc == null) || (arriveLoc == null)) {
					handleXMLError(-2);	return;
				}
				leg.departLoc = departLoc;
				leg.arriveLoc = arriveLoc;
					// we estimate the flight path bounds here so we can compute any required horizontal shifting of the map
					// the height of the bounding rect needs to be calculated 
		    	gCurrentHRange = growHRangeToIncludeLocation(gCurrentHRange,departLoc);
		    	gCurrentHRange = growHRangeToIncludeLocation(gCurrentHRange,arriveLoc);
		    	gCurrentVRange = growVRangeToIncludeLocation(gCurrentVRange,departLoc);
		    	gCurrentVRange = growVRangeToIncludeLocation(gCurrentVRange,arriveLoc);
				departLocs = departLocs.concat(departLoc);
				arriveLocs = arriveLocs.concat(arriveLoc);
			}
		}		
	}

	gOverallFlightStatus = resolveOverallFlightStatus(firstLeg,firstSchedLeg,enrouteLeg,lastLandedLeg);
	gDepartLocs = departLocs;
	gArriveLocs = arriveLocs;
	
	drawFlightPath();
	
	updateStatusInfo(gOverallFlightStatus,trackingFlightNo,legCount);
	
	defaultArriveCity = lastLeg.arrive.airportCode;
	defaultDepartCity = firstLeg.depart.airportCode;
	defaultAirlineCode = flight.airlineCode;

	updateCombosWithDefaults();
	savePrefsFromDefaults();
	if (alreadyZoomed == false) setTimeout('zoomMap("in");',1500);
}

function updateStatusInfo(statusInfo,trackingFlightNo,legCount)
{	
	var leg = (statusInfo.status == "Stopped" ? statusInfo.nextLeg : statusInfo.leg);
	var departInfo = leg.depart;
	var arriveInfo = leg.arrive;
	var flightDuration = statusInfo.leg.duration;
	var tracking = statusInfo.leg.tracking;

	var progressRatio = null;

	if ((window.widget && departInfo.best != null) && (departInfo.best != null )) {
		var departDate = getBestDateFromInfo(departInfo);
		var arriveDate = getBestDateFromInfo(arriveInfo);
	    var departTime = formatDateForDisplay(departDate,true);
	    var arriveTime = formatDateForDisplay(arriveDate,true);

		var departTimeLabel = document.getElementById('departTime-label');
		var arriveTimeLabel = document.getElementById('arriveTime-label');
		departTimeLabel.innerText = (legCount <= 1 ? "" : departInfo.airportCode+" ")+departTime;
		arriveTimeLabel.innerText = (legCount <= 1 ? "" : arriveInfo.airportCode+" ")+arriveTime;

		shrinkToFit(departTimeLabel, 80, 9.5);
		shrinkToFit(arriveTimeLabel, 80, 9.5);

		var nowDate = new Date();
		
		// Use plugin to calculate elapsed time, duration
		
		departDateMSecs = departDate.date.getTime();
		arriveDateMSecs = arriveDate.date.getTime();
		nowDateMSecs = nowDate.getTime();
		
		var timeIntoFlight = nowDateMSecs-departDateMSecs;
		var totalFlightTime = arriveDateMSecs-departDateMSecs;
		
		progressRatio = timeIntoFlight/totalFlightTime;
		if (progressRatio < 0)
			progressRatio = 0;
		statusInfo.progressRatio = progressRatio;
	}
	if (statusInfo.status == 'Enroute') {
		if (!statusInfo.isPlaneLocationAllowed) {
			document.getElementById('airAltitudeUnits-label').innerText = getLocalizedString("PlaneLocationNotProvided");
		} else if (tracking != null) {
			var	measurementSystem = getLocalizedString("MerasurementSystem");
			var	useMKS = false;
			if (measurementSystem == "Metric")
				useMKS = true;
			if ((tracking.speed != null) && (tracking.speed > 0)) {     // If we have speed data, display it
				var	speed = (useMKS ? tracking.speed * 1.6093 : tracking.speed);
				document.getElementById('airSpeed-label').innerText = Math.round(speed);
				document.getElementById('airSpeedUnits-label').innerText = getLocalizedString("SpeedUnits");
			}
			if ((tracking.altitude != null) && (tracking.altitude > 0)) {  // If we have altitude data, display it
				var	altitude = (useMKS ? tracking.altitude * 0.3048 : tracking.altitude);
				document.getElementById('airAltitude-label').innerText = Math.round(altitude);
				document.getElementById('airAltitudeUnits-label').innerText = getLocalizedString("DistanceUnits");
			}
		} else {
			// If we don't have speed or altitude information, chances are we are approximating this flight path
			 document.getElementById('airAltitudeUnits-label').innerText = getLocalizedString("Approximate Flight Path");
		}
	}

	var flightGateLabel = document.getElementById('flightGate-label');
	if (statusInfo.status == "Scheduled" ) {
		flightGateLabel.innerText = getLocalizedString("FlightShort") + " " + trackingFlightNo + " "
									+ getLocalizedString("DepartingShort") + " "
									+ getLocalizedString("Terminal") + " " + departInfo.terminal
									+ (departInfo.gate!="N/A"?(getLocalizedString("Gate")+" "+ departInfo.gate):"");
		if ( departInfo.terminal == "N/A" )
			flightGateLabel.innerText = getLocalizedString("FlightShort") + " " + trackingFlightNo;
	} else if (statusInfo.status == "Stopped" ) {
		flightGateLabel.innerText = getLocalizedString("FlightShort") + " " + trackingFlightNo + " "
									+ getLocalizedString("DepartingShort") + " "
									+ getLocalizedString("Terminal") + " " + statusInfo.nextLeg.depart.terminal
									+ (statusInfo.nextLeg.depart.gate!="N/A"?(getLocalizedString("Gate")+" "+ statusInfo.nextLeg.depart.gate):"");
		if ( statusInfo.nextLeg.depart.terminal == "N/A" )
			flightGateLabel.innerText = getLocalizedString("FlightShort") + " " + trackingFlightNo;
	} else if (statusInfo.status == "Enroute" || statusInfo.status == "Landed" ) {
		flightGateLabel.innerText = getLocalizedString("FlightShort") + " " + trackingFlightNo + " " 
									+ getLocalizedString("ArrivingShort") + " " 
									+ getLocalizedString("Terminal") + " " + arriveInfo.terminal 
									+ (arriveInfo.gate!="N/A"?(", " + getLocalizedString("Gate") +" "+ arriveInfo.gate):"");
		if ( arriveInfo.terminal == "N/A" )
			flightGateLabel.innerText = getLocalizedString("FlightShort") + " " + trackingFlightNo;
	}

	flightGateLabel.style.fontSize = "11px";
	shrinkToFit(flightGateLabel, 145);

		// Check to see if the plane is running late or early 
	var timeDiscrepencyString = checkTimeDiscrepencies(statusInfo);
	document.getElementById('flightStatus-label').innerText = timeDiscrepencyString;

	if ( statusInfo.status != "Enroute" ) {
		document.getElementById('airSpeed-label').innerText = "";
		document.getElementById('airSpeedUnits-label').innerText = "";
		document.getElementById('airAltitude-label').innerText = "";
		
		if ( statusInfo.status == "Landed" )
			document.getElementById('airAltitudeUnits-label').innerText = getLocalizedString('Flight Landed');
		else
			document.getElementById('airAltitudeUnits-label').innerText = "";
	}
}

function checkTimeDiscrepencies(statusInfo) 
{
	var leg = statusInfo.leg;
	var	statusString = "";
	var schDate;
	var curDate;
	var statusStr = "";

	switch (statusInfo.status) {
		case "Scheduled":
			schDate = leg.depart.scheduledDate;
			curDate = leg.depart.bestDate;
			break;
		case "Landed":
			schDate = leg.arrive.scheduledDate;
			curDate = leg.arrive.bestDate;
			break;
		case "Enroute":
			schDate = leg.arrive.scheduledDate;
			curDate = leg.arrive.bestDate;
			break;
		case "Stopped":
			schDate = statusInfo.nextLeg.depart.scheduledDate;
			curDate = statusInfo.nextLeg.depart.bestDate;
			statusString = getLocalizedString('Dpt:')+" ";
			break;
		default:	return statusString;	break;
	}
	var msecLate  = curDate.date.getTime() - schDate.date.getTime();
	var minutesLate = Math.round(msecLate/60000.0);
	if (minutesLate < 0) {
		statusString += formatMinutesTime(-minutesLate)+" "+getLocalizedString('Early').toLowerCase();
	} else 	if (minutesLate > 0) {
		statusString += formatMinutesTime(minutesLate)+" "+getLocalizedString('Late').toLowerCase();
	} else {
		statusString += getLocalizedString('On Time').toLowerCase();
	}
	return statusString;
}

var zoomAnimation = {duration:0, starttime:0, to:1.0, now:0.0, from:0.0, timer:null};

function zoomMap(inOrOut) {
    if ( zoomInterval != null ) 
    {
        clearInterval(zoomInterval);
        zoomInterval = null;
    }
    if ( zoomAnimation.timer != null ) 
    {
        clearInterval(zoomAnimation.timer);
        zoomAnimation.timer = null;
    }
    
    alreadyZoomed = true;
    
    var starttime = (new Date).getTime() - 40; // set it back one frame
        
    if ( zoomAnimation.now == 0.0 || zoomAnimation.now == 1.0 )
        zoomAnimation.now = (inOrOut == "in" ? 1.0 : 0.0);
    
    zoomAnimation.duration = 1200;
    zoomAnimation.starttime = starttime;
    zoomAnimation.timer = setInterval ("zoomStep();", 40);
    zoomAnimation.from = zoomAnimation.now;
    zoomAnimation.to = (inOrOut == "in" ? 0.0 : 1.0);
    
    // Calculate centered bounding rect for both points
    calcZoomSourceRect();
}

function resetZoom() {
	alreadyZoomed = false;
    gZoomRect = {x:0.0, y:0.0, width:kMapWidth, height:kMapHeight};
    gZoomSrcRect = {x:0.0, y:0.0, width:kMapWidth, height:kMapHeight};
}

function calcZoomSourceRect() {
	var	right = gCurrentHRange.right;
	var left = gCurrentHRange.left;
	var top = gCurrentVRange.top;
	var bottom = gCurrentVRange.bottom;
	flightPathWidth = right - left;
	flightPathHeight = bottom - top;
	if ((flightPathWidth/flightPathHeight) > (kMapWidth/kMapHeight)) {
		gZoomSrcRect.x = left-flightPathWidth*.15;
		gZoomSrcRect.width = flightPathWidth*1.30;
		gZoomSrcRect.height = flightPathWidth*(kMapHeight/kMapWidth)*1.30;
		gZoomSrcRect.y = top - (gZoomSrcRect.height-flightPathHeight*1.30)/2
	} else {
		gZoomSrcRect.y = top - flightPathHeight*.15;
		gZoomSrcRect.height = flightPathHeight*1.30;
		gZoomSrcRect.width = flightPathHeight*(kMapWidth/kMapHeight)*1.30;
		gZoomSrcRect.x = left - (gZoomSrcRect.width-flightPathWidth*1.30)/2
	}
	if (gZoomSrcRect.y < 0)
		gZoomSrcRect.y = 0;
}

function zoomStep() {
    var T;
	var ease;
	var time = (new Date).getTime();

	T = limit_3(time - zoomAnimation.starttime, 0, zoomAnimation.duration);
	
	if (T >= zoomAnimation.duration) {
		clearInterval (zoomAnimation.timer);
		zoomAnimation.timer = null;
		zoomAnimation.now = zoomAnimation.to;
	}
	else {
		ease = 0.5 - (0.5 * Math.cos(Math.PI * T / zoomAnimation.duration));
		zoomAnimation.now = nextFloat(zoomAnimation.from, zoomAnimation.to, ease);
	}
	
	gZoomRect.x = gZoomSrcRect.x * (1.0 - zoomAnimation.now) + zoomAnimation.now*gCurrentXOffset;
	gZoomRect.y = gZoomSrcRect.y * (1.0 - zoomAnimation.now);
	gZoomRect.width = gZoomSrcRect.width + ((kMapWidth - gZoomSrcRect.width) * zoomAnimation.now);
	gZoomRect.height = gZoomSrcRect.height + ((kMapHeight - gZoomSrcRect.height) * zoomAnimation.now);
	
	drawFlightPath();
}


var searchAnimation = {now:0.0, val:0.0, step:0.0, duration:0.0, element:null, timer:null};
function searchAnimate() {
    if ( searchAnimation.val >= searchAnimation.duration ) {
        clearInterval(searchAnimation.timer);
        searchAnimation.timer = null;
        searchAnimation.element.style.opacity = 1.0;
    }
    else {
        searchAnimation.val += searchAnimation.step;
        searchAnimation.now = 0.5 - (0.5 * Math.cos(searchAnimation.val));
        searchAnimation.element.style.opacity = animation.now;
    }
}

function populateAirportSelect()
{
  	gDepartCombo.initAsAirportMenu();
  	gArriveCombo.initAsAirportMenu();
}

function populateAirlineSelect()
{ 
  	gAirlineCombo.initAsAirlineMenu(getLocalizedString('All Airlines'));
    gAirlineCombo.setSelectedIndex(0);
    gAirlineCombo.setValue(getLocalizedString('All Airlines'));
}


function createkey(key) {
	return widget.identifier + "-" + key;
}

function makeAirlineInfo(airlineData)
{
	return (airlineData == null ? null : {code:airlineData[0], name:airlineData[1]});
}

function airlineForCode(code) {
	var	airlineData = gDepartCombo.dataForAirlineCode(code);
	return makeAirlineInfo(airlineData);
}

function makeAirportInfo(airportData)
{
	return (airportData == null ? null : {code:airportData[0],name:airportData[1],city:airportData[2],state:airportData[3],country:airportData[4]});
}

function airportForCode(code) {
	var	airportData = gAirlineCombo.dataForAirportCode(code);
	dumpToString(airportData);
    return makeAirportInfo(airportData);
}

function airportFromCombo(combo) { 
	return combo.selectedCode();
}

function airlineFromCombo(combo) {
	return combo.selectedCode();
}

function comboTargetActionTriggered()
{
	if (gDebug > 0)	alert("comboTargetActionTriggered.js");
	if (canDoAction()) {
		savePreferencesFromCombos();
		button = document.getElementById('actionButton-label');
		if (gDebug > 0) alert("comboTargetActionTriggered: "+button);
		if ( applicationMode == kFindMode ) {
			if (button.innerText == getLocalizedString('Track Flight'))
				trackFlightWithResultIndex(currentSelectionIdx);
			else if (button.innerText == getLocalizedString('Find Flights'))
				fetchFlightData();
			else alert('comboTargetActionTriggered: bogus button text');
		}
		else { //if (applicationMode == kStartMode) {
			fetchFlightData();
		}
	}
}

function airlineComboTargetActionTriggered()
{
    var newAirlineCode = airlineFromCombo(gAirlineCombo);
    defaultAirlineCode = newAirlineCode;
    lastSelectedAirlineCode = defaultAirlineCode;
    if ((!newAirlineCode) || (newAirlineCode.length == 0)) {
    	gAirlineCombo.setValue(getLocalizedString("All Airlines"));
		gAirlineCombo.setSelectedIndex(0);
    }

    updateActionButtonEnabled();
    comboTargetActionTriggered();
}

function airlineTextChanged(newValue)
{
	//if someone starts typing, clear the flight they're looking at
	if(applicationMode != kStartMode)
	{
		clearTrackingFlightNumber();
		updateUIWithMode(kStartMode);	
	}

    var newAirlineCode = gAirlineCombo.selectedCode();
    if (!newAirlineCode)
    	newAirlineCode = null;
    document.getElementById('status-label').innerText = "";
 	stopServerAccessAnimation();
   if ( lastXMLRequest != null )
        lastXMLRequest.abort();
	if ((defaultAirlineCode != newAirlineCode))
	{
		if (gDebug > 0) alert("airlineTextChanged "+defaultAirlineCode+" ==> "+newAirlineCode);
		document.getElementById('actionButton-label').innerText = getLocalizedString('Find Flights');
        defaultAirlineCode = newAirlineCode;
        if (defaultAirlineCode == undefined)
	        defaultAirlineCode = null;
	    lastSelectedAirlineCode = defaultAirlineCode;

		if (window.widget) {
			widget.setPreferenceForKey (defaultAirlineCode, "defaultAirlineCode");
		}
		
		updateActionButtonEnabled();
	}
}

function departComboTargetActionTriggered()
{
    defaultDepartCity = airportFromCombo(gDepartCombo);
    updateActionButtonEnabled();
    comboTargetActionTriggered();
}

function arriveComboTargetActionTriggered()
{
	defaultArriveCity = airportFromCombo(gArriveCombo);
	updateActionButtonEnabled();
    comboTargetActionTriggered();
}

function cityTextChanged (combo, currentCity, updatePrefFunc) 
{
	// if someone starts typing, clear the flight they're looking at
	if(applicationMode != kStartMode)
	{
		clearTrackingFlightNumber();
		updateUIWithMode(kStartMode);
	}
		
	
	var newAirport = combo.selectedCode();
	var newAirportString = combo.value();
	
    document.getElementById('status-label').innerText = "";
	stopServerAccessAnimation();
    if ( lastXMLRequest != null )
        lastXMLRequest.abort();
	if ((currentCity != newAirport))
	{	
		document.getElementById('actionButton-label').innerText = getLocalizedString('Find Flights');
		updatePrefFunc(newAirport);					
        updateActionButtonEnabled();		
	}
}

function departCityTextChanged () {
	cityTextChanged (gDepartCombo, defaultDepartCity, function(city){defaultDepartCity = city});
}

function arriveCityTextChanged () {
	cityTextChanged (gArriveCombo, defaultArriveCity, function(city){defaultArriveCity = city});
}

function swapAirports() {

    if ( defaultDepartCity == null || defaultArriveCity == null || applicationMode == kTrackMode )
        return;

    var newDepartIndex = gDepartCombo.selectedIndex();
    var newArriveIndex = gArriveCombo.selectedIndex();
    
    if ( (newArriveIndex != -1) && (newDepartIndex != -1 )) {
        gDepartCombo.setSelectedIndex(newDepartIndex);
        gArriveCombo.setSelectedIndex(newArriveIndex);
        
        departCityTextChanged();
        arriveCityTextChanged();
    }
}

function actionBtnMouseDown(button) {
	if (canDoAction()) {
		if ((applicationMode != kFindMode) || (currentSelectionIdx >= 0)) {
		    document.getElementById('actionButton-label').style.color = "#FFBA00";
    		document.getElementById('actionButton-label').setAttribute("pressed","true");
    	}
    }
}

function actionBtnMouseUp(button) {
	if (canDoAction()) {
		var pressed = document.getElementById('actionButton-label').getAttribute("pressed");
		if ( (pressed != null) && (pressed == "true")) {
			if ( applicationMode == kFindMode ) {
				if (currentSelectionIdx >= 0) {
					var trackFlightStr = getLocalizedString('Track Flight');
					var findFlightStr = getLocalizedString('Find Flights');
					var buttonText = button.innerText;
					if (buttonText.indexOf(trackFlightStr) >= 0) {
						trackFlightWithResultIndex(currentSelectionIdx);
					} else if (buttonText.indexOf(findFlightStr) >= 0) {
						// searchFlights(currentSelectionIdx);
						fetchFlightData();
					} else {
						alert('actionBtnMouseUp: bogus\nbutton text:'+buttonText+':for mode:'+applicationMode+":"+trackFlightStr+":"+findFlightStr);
					}
				}
			}
			else if ( applicationMode == kStartMode ) {
				fetchFlightData();
			}
		}
		if ((applicationMode != kFindMode) || (currentSelectionIdx >= 0)) {
			document.getElementById('actionButton-label').style.color = "white";
			document.getElementById('actionButton-label').setAttribute("pressed","false");
		}
   	}
}

function backArrowMouseDown(button) {
    button.src = "Images/backArrow_pressed.png";
    button.setAttribute("pressed","true");
}

function backArrowMouseUp(button) {

    if ( (button.getAttribute("pressed") != null) && (button.getAttribute("pressed") == "true" )) {
		stopServerAccessAnimation();
        updateUIWithMode(applicationMode-1);

 		//start update in case they were tracking for a long time.
		if(applicationMode == kFindMode)
		{
			gHighlightFlightNumberWhenListLoads = trackingFlightNo; //for highlighting in list
			clearTrackingFlightNumber();
			if(searchResults.length)
				setupUpdates(false);	
			else
				fetchFlightData();
		}
    }

    button.src = "Images/backArrow_button.png";
    button.setAttribute("pressed","false");
}

function canvasClick(event) 
{
    if ( applicationMode == kTrackMode ) {
        if ( zoomAnimation.to == 0.0 ) 
            zoomMap("out");
        else 
            zoomMap("in");
    }
}


function formatDateForDisplay(dateInfo,shortForm)
{
	var retVal = "N/A";
	if (shortForm == null)
		shortForm = false;
	if (dateInfo != null) {
		var diffStr = "";
		if (dateInfo.dayOffset != null) {
			var diff = dateInfo.dayOffset;
			if (diff > 0)
				diffStr = "+"+diff+(shortForm ? "" : getLocalizedString('DayLowerCase'));
			else if (diff < 0)
				diffStr = ""+diff+(shortForm ? "" : getLocalizedString('DayLowerCase'));
		}
		var date = new Date();
		var tzOffset = dateInfo.tzOffset - date.getTimezoneOffset()*60000;
		date.setTime(dateInfo.date.getTime() - tzOffset);
		if (date != null) {
			var timeStr = date.toLocaleTimeString("short");
			if (timeStr != "Invalid Date") {
				retVal = timeStr;
			}
		}
	}
	return retVal;
}

var k1DayMS = 24*3600*1000;
var k1HourMS = 3600*1000;

function createDateFromServerString(serverDate)
{
// We're going to be stupid and hard coded about parsing the server provided date strings for now
// ??? for some reason parseInt returns 0 on this but parsefloat works ???
	var year   		= parseFloat(serverDate.substr(0,4));
	var month  		= parseFloat(serverDate.substr(5,2));
	var day 		= parseFloat(serverDate.substr(8,2));
	var hour   		= parseFloat(serverDate.substr(11,2));
	var min    		= parseFloat(serverDate.substr(14,2));
	var sec			= parseFloat(serverDate.substr(17,2));
	var tzhourStr	= serverDate.substr(20,2)
	var tzhour  	= parseFloat(tzhourStr);	
	var tzmin		= parseFloat(serverDate.substr(23,2));
	var tzsign  =	 serverDate.substr(19,1);
	var tzOffsetMS = 0;
	var retVal = null;
	if (isNaN(year) || isNaN(month) || isNaN(day) || isNaN(hour) || isNaN(min)  || isNaN(sec)) {
		retVal = {date:new Date(Infinity),tzOffset:null, dayOffset:null};
	} else {
		var date = new Date();
		// here we do proper handling of the timezone offsets.  
		// We enter everything in pretending it's UTC time
		// and then we convert it to miliseconds since 1970,
		// add the timezone offset in miliseconds to it
		// and then set this as the new UTC time
		date.setUTCFullYear(year);
		date.setUTCMonth(month-1);
		date.setUTCDate(day);
		date.setUTCHours(hour);
		date.setUTCMinutes(min);
		date.setUTCSeconds(sec);
		var utcTimeMS = date.getTime();
		if (!isNaN(tzhour) && !isNaN(tzmin)) {
			tzOffsetMS = tzhour * 3600 * 1000 + tzmin*60*1000;
			if (tzsign == "+")
				tzOffsetMS *= -1;
		}
		// apply timezone offset
		utcTimeMS +=  tzOffsetMS;
		date.setTime(utcTimeMS);
		// compute day diff
		var otherLocalTime = new Date();
		var localTimeZoneOffset = date.getTimezoneOffset()*60000;
		var totalTzOffset 		= tzOffsetMS-localTimeZoneOffset;
		var otherLocalMS		= otherLocalTime.getTime() - tzOffsetMS;
		var dateLocalMS			= utcTimeMS - tzOffsetMS;
		var previousOtherMidnight 	= k1DayMS*Math.floor(otherLocalMS/k1DayMS);
		var previousDateMidnight 	= k1DayMS*Math.floor(dateLocalMS/k1DayMS);
		var otherLocalDayNumber	= Math.floor(otherLocalMS/k1DayMS);
		var dateDayNumber 		= Math.floor(dateLocalMS/k1DayMS);
		var dayDiff = dateDayNumber-otherLocalDayNumber;

		retVal = {date:date,tzOffset:tzOffsetMS,dayOffset:dayDiff};
	}
	return retVal;
}

function dateForLegInfoWithAttribute(legInfo,attribute)
{
	var serverDateString = legInfo[attribute];
	return createDateFromServerString(serverDateString);
	// eval("legInfo."+attribute+"Date = dateInfo;");
	// var newAttrName = attribute+'Date';
}

function createFlightTableEntry(airlineName,airlineCode,flightNumber,departDates,arriveDates,cities,overallFlightStatus)
{
       var ftEntry = new Array();
        
        var topRow = document.createElement("div");
        var bottomRow = document.createElement("div");
        topRow.setAttribute("class", "row topRow");
        bottomRow.setAttribute("class", "row bottomRow");
        
        var airlineSpan = document.createElement("span");
        airlineSpan.setAttribute("class", "row topRow tblAirline");
        var alName = airlineName;

        airlineSpan.innerText = alName;
        airlineSpan.setAttribute("code", airlineCode);
        airlineSpan.setAttribute("departCity",defaultDepartCity);
        airlineSpan.setAttribute("arriveCity",defaultArriveCity);
        topRow.appendChild(airlineSpan);

        var flightNoSpan = document.createElement("span");
        flightNoSpan.setAttribute("class","row topRow tblFlightNo");
        flightNoSpan.innerText = flightNumber;
        topRow.appendChild(flightNoSpan);
        
        var statusSpan = document.createElement("span");
        statusSpan.setAttribute("class", "row topRow tblStatus");
        
		// Stopped is usful internally, but Enroute is a better message to show
		var statusString = (overallFlightStatus.status == "Stopped") ? "Enroute" : overallFlightStatus.status;
        statusSpan.innerText = getLocalizedString(statusString);
        topRow.appendChild(statusSpan);

		var	now = new Date();
		var departDate = departDates[0];
		var arriveDate = arriveDates[arriveDates.length-1];
		var useShortDateFormat = (cities.length>1) || 
								 (	(	(Math.abs(departDate.dayOffset)+Math.abs(arriveDate.dayOffset)) > 1)
								 	&& 	(cities.length>0)
								 );
		var departGateTime = formatDateForDisplay(departDate,useShortDateFormat);
		var arriveGateTime = formatDateForDisplay(arriveDate,useShortDateFormat);
		
		var flightInfoSpan = document.createElement("span");
        flightInfoSpan.setAttribute("class", "row bottomRow tblFlightInfo");
        flightInfoSpan.innerText = 	(cities.length > 1 ? getLocalizedString('D')+" " : getLocalizedString('Departs '))+departGateTime+" "+
        						  	(cities.length > 1 ? getLocalizedString('A')+" " : getLocalizedString('Arrives '))+arriveGateTime+" "+
        						  	(cities.length > 0 ? getLocalizedString('Via').toLowerCase()+" "+cities.toString() : "");
        bottomRow.appendChild(flightInfoSpan);
    
        ftEntry.push(topRow);
        ftEntry.push(bottomRow);
        ftEntry.push(departDate);
        ftEntry.push(arriveDate);
        return ftEntry;
}


function resolveOverallFlightStatus(firstLeg,scheduledLeg,enrouteLeg,landedLeg)
{
	if (enrouteLeg != null)
		return {status:'Enroute', leg:enrouteLeg};
	else if ((landedLeg != null) && (scheduledLeg != null) && (landedLeg.legNumber < scheduledLeg.legNumber))
		return {status:'Enroute', leg:landedLeg, nextLeg:scheduledLeg}; //The overall flight is enroute
	else if (landedLeg != null)
		return {status:'Landed', leg:landedLeg};
	else if (scheduledLeg != null)
		return {status:'Scheduled', leg:scheduledLeg};
	else
		return {status:firstLeg.status, leg:firstLeg};
}

function getBestDateFromInfo(legInfo)
{
	if ((legInfo.bestDate != null) && (legInfo.best != 'N/A'))
		return legInfo.bestDate;
	else
		return legInfo.scheduledDate;
}

function searchResultsLoaded(data, extraInfo)
{
	stopServerAccessAnimation();
	if (gDebug > 0)
		alert("searchResultsLoaded"+data);
    if ( data == null) {
    	if (gDebug > 0)
	        alert("extraInfo = "+extraInfo); 
	    return;
    }
   
    document.getElementById('status-label').innerText = "";
    
    // remove all children
	while (searchResults.length) searchResults.pop();
		
    // handle error
    if ( handleXMLError(data.error) ) return;

    var flightList = data.flights; 
    for ( var i = 0; i < flightList.length; i++ ) 
    {
        var flight = flightList[i];
        var fAirlineName = flight.airlineName;
        var fAirlineCode = flight.airlineCode;
        var fNumber = flight.number;
        var departDates = new Array();
        var arriveDates = new Array();
        var cities = new Array();

		var	firstLeg = null;
		var	firstSchedLeg = null;
		var	lastLandedLeg = null;
		var enrouteLeg = null;
		var overallFlightStatus = null;
        for ( var j = 0; (flight.legs != null) && (j < flight.legs.length); j++ )
        {
            var leg = flight.legs[j];
            var flightStatus = leg.status;
            leg.legNumber = j;
			if (firstLeg == null)
				firstLeg = leg;
			if (flightStatus == "Enroute")
				enrouteLeg = leg;
			else if (flightStatus == "Landed")
				lastLandedLeg = leg;
			else if ((flightStatus == "Scheduled") && (firstSchedLeg == null)) 
				firstSchedLeg = leg;
			
            leg.depart.scheduledDate = dateForLegInfoWithAttribute(leg.depart,'scheduled');
			leg.depart.bestDate = dateForLegInfoWithAttribute(leg.depart,'best');
			leg.arrive.scheduledDate = dateForLegInfoWithAttribute(leg.arrive,'scheduled');
			leg.arrive.bestDate = dateForLegInfoWithAttribute(leg.arrive,'best');
			
            var departServerDate = getBestDateFromInfo(leg.depart);
            var arriveServerDate = getBestDateFromInfo(leg.arrive);
			departDates.push(departServerDate);
			arriveDates.push(arriveServerDate);
			if (j > 0) {
				cities.push(leg.depart.airportCode);
			}
	    }
	   	overallFlightStatus = resolveOverallFlightStatus(firstLeg,firstSchedLeg,enrouteLeg,lastLandedLeg);
		var findResult = createFlightTableEntry(fAirlineName,fAirlineCode,fNumber,departDates,arriveDates,cities,overallFlightStatus);
        
		if(fNumber == gHighlightFlightNumberWhenListLoads)
			currentSelectionIdx = searchResults.length;
		searchResults.push(findResult);
    }
    if (gDebug > 0) alert("end of searchResultsLoaded");

    updateUIWithMode(kFindMode);
    populateResultTable();
    if (gDebug > 0) alert("DONE w/ searchResultsLoaded");
    updateActionButtonEnabled();
}

function populateResultTable() {
    var view = document.getElementById('view');

	while (view.hasChildNodes()) {
        if ( view.firstChild.getAttribute("class") == "activeFindResult" && currentSelectionIdx < 0 ) 
			currentSelectionIdx = parseInt(view.firstChild.getAttribute("row"));
        view.removeChild(view.firstChild);
	}

	var firstRow = null;
    // add the new results
    var resultsCount = searchResults.length;
    for(var k=0; k < resultsCount; k++) {
        var rowDiv = document.createElement("div");
        
		if (k == currentSelectionIdx ) {
            rowDiv.setAttribute("class","activeFindResult");
            currentSelection = rowDiv;
        }
        else 
			rowDiv.setAttribute("class","findResult");
        
		rowDiv.setAttribute("row",k);
        rowDiv.onclick = function(event) {clickOnRow(event,this)};
        rowDiv.ondblclick = function(event) {trackFlightWithResultIndex(currentSelectionIdx);};
        
        var spanC = searchResults[k].length-2;
        for ( var j = 0; j < spanC; j++ ) {
            rowDiv.appendChild((searchResults[k])[j]);
        }
        if (k == 0)
        	firstRow = rowDiv;
        view.appendChild(rowDiv);
    }
    
    gDocumentHeight = parseFloat(document.defaultView.getComputedStyle(view, '').getPropertyValue('height'));
	gViewHeight = parseFloat (document.defaultView.getComputedStyle(view.parentNode, '').getPropertyValue('height'));
	
	var percent = getProportion (gViewHeight, gDocumentHeight);
	var thumb = document.getElementById('scroll');
	var channel = document.getElementById('channel');
	
	if (percent == 0) {
		thumb.style.display = 'none';
		channel.style.display = 'none';
	} else {
		var newHeight = Math.round((scrollBarHeight) * percent); // should I round or floor
		
		thumb.style.display = 'block';
		channel.style.display = 'block';
		document.getElementById('scroll-mid').style.height = newHeight + "px";
	}
	
	view.style.top = "0";
	thumb.style.top=""; // reset
    detailArea.style.display = "block";
    if (!currentSelection && firstRow != null) {
      	clickOnRow(null,firstRow);
   	}
	gHighlightFlightNumberWhenListLoads = null;
}

function clearCurrentSelection () {
	if (currentSelection != null) {
		currentSelection.setAttribute("class", "findResult");
	}
	currentSelection = null;
	currentSelectionIdx = -1;
}



function clickOnRow(event, clickedRow) {

	// change the current selection

	if (currentSelection != null) {
		currentSelection.setAttribute("class", "findResult");
	}
	currentSelection = clickedRow;
	currentSelectionIdx = parseInt(clickedRow.getAttribute("row"));
	currentSelection.setAttribute("class", "activeFindResult");

    var view = document.getElementById('view');
    gDocumentHeight = parseFloat(document.defaultView.getComputedStyle(view, '').getPropertyValue('height'));
	gViewHeight = parseFloat (document.defaultView.getComputedStyle(view.parentNode, '').getPropertyValue('height'));
	var percent = getProportion (gViewHeight, gDocumentHeight);
	if (percent == 0) {
		currentSelection.style.width = 225+"px";
	} else {
		currentSelection.style.width = 214+"px";
	}
	
	document.getElementById('actionButton-label').innerText = getLocalizedString('Track Flight');
	updateActionButtonEnabled();
}

function clickOnHeader(header) {
	var headerClass = header.getAttribute("class");
    var headerSortCtl = header.getElementsByTagName("img")[0];
    
    if ( headerSortCtl.style.display == "inline-block" ) {
        if ( header.getAttribute("sort") == "Down" ) {
            headerSortCtl.src = "Images/sortUp.png";
            header.setAttribute("sort", "Up");
        } else {
            headerSortCtl.src = "Images/sortDown.png";
            header.setAttribute("sort", "Down");
        }
        
        searchResults.reverse();
        populateResultTable();
        return;
    }

    var spans = document.getElementsByTagName("span");
    var c = spans.length;
    for ( i = 0; i < c; i++ ) {
        if ( spans[i].getAttribute("class") && 
             (spans[i].getAttribute("class").indexOf("Header") > -1) )
            spans[i].getElementsByTagName("img")[0].style.display = "none";
    }
    
    header.getElementsByTagName("img")[0].style.display = "inline-block";
    
    if ( headerClass.indexOf("airlineHeader") > -1 )
        searchResults.sort();
    else if ( headerClass.indexOf("flightNoHeader") > -1 )
        searchResults.sort(sortFlightNos);
    else if ( headerClass.indexOf("statusHeader") > -1 ) {
        var statusSort = function(a,b) {sortStatus(a,b);};
        //var departSort = function(a,b) {sortDates(a[2].innerHTML,b[2].innerHTML)};
        //searchResults.sort(departSort);
    }
    else if ( headerClass.indexOf("arriveHeader") > -1 ) {
        var arriveSort = function(a,b) {sortDates(a[3].innerHTML,b[3].innerHTML)};
        searchResults.sort(arriveSort);
    }
    
    if ( header.getAttribute("sort") == "Up" ) 
        searchResults.reverse();
    
    populateResultTable();
}

function sortFlightNos(a,b) {
    return parseInt(a[1].innerHTML) - parseInt(b[1].innerHTML);
}

function sortStatus(a,b) {
    return a[2].localeCompare(b[2]);
}

function sortDates(a,b) {
    var monthA = parseInt(a.substr(0,2),10);
    var monthB = parseInt(b.substr(0,2),10);
    var dayA = parseInt(a.substr(3,2),10);
    var dayB = parseInt(b.substr(3,2),10);
    var hourA = parseInt(a.substr(6,2),10);
    var hourB = parseInt(b.substr(6,2),10);
    var minsA = parseInt(a.substr(9,2),10);
    var minsB = parseInt(b.substr(9,2),10);
    
    if ( monthA - monthB == 0 ) {
        if ( dayA - dayB == 0 ) {
            if ( hourA - hourB == 0 ) {
                return minsA - minsB;
            }
            else  {
                return hourA - hourB;
            }
        }
        else return dayA - dayB;
    }
    else return monthA - monthB;
}

function getProportion (viewheight, documentheight)
{
	if (documentheight <= viewheight)
		return 0;
	else
		return viewheight/documentheight;
}

var gStartY = -1;
var gScrollThumb = null;
var gScrollThumbHeight = 0;
var gScrollThumbTop = 0;
var kTopOfScrollbar = 6
var kWindowToViewOffset = 4;
var kBottomOfScrollbar = 155; //156 169;
var gNumberOfScrollablePixels;

function computeScrollParameters()
{
	gScrollThumb = document.getElementById('scroll');
	gScrollThumbHeight = parseInt(document.getElementById('scroll-mid').style.height);
	gScrollThumbTop    = parseInt(document.defaultView.getComputedStyle(gScrollThumb,'').getPropertyValue('top'));
	gNumberOfScrollablePixels = kBottomOfScrollbar - gScrollThumbHeight - kTopOfScrollbar-kWindowToViewOffset;
}

function scrollLineUpDown(delta)
{
    var resultsCount = searchResults.length
	
	var topOfThumb    = gScrollThumbTop;
	var bottomOfThumb = gScrollThumbTop + gScrollThumbHeight;
	var deltaY = 0;
	
	var increment = gNumberOfScrollablePixels/resultsCount;
	deltaY = delta*increment;
	var firstElement = searchResults[0];
	if (firstElement != null) {
		var elementHeight = heightOfElement(firstElement);
		var elementDelta = gNumberOfScrollablePixels*elementHeight / (gDocumentHeight - gViewHeight);
		if ((deltaY != 0) && (Math.abs(deltaY) < elementDelta))
			if (deltaY > 0)
				deltaY = elementDelta;
			else
				deltaY = - elementDelta;
	}	
    if (gDebug > 0)
    		alert( "deltaY: " + deltaY);

    if ( deltaY != 0 )
    {
        var newPosition = topOfThumb +deltaY;
        if (newPosition < kTopOfScrollbar)
        {
            newPosition = kTopOfScrollbar;
        }
        else if ((newPosition + gScrollThumbHeight+kWindowToViewOffset) > kBottomOfScrollbar)
        {
            newPosition = kBottomOfScrollbar - gScrollThumbHeight-kWindowToViewOffset;
        }
    
        gScrollThumb.style.top = newPosition + "px";
        scrollColumn(newPosition - kTopOfScrollbar, gScrollThumb.id);
    }
}

function scrollChannelClick(event)
{	
	computeScrollParameters();
	var topOfThumb    = gScrollThumbTop;
	var bottomOfThumb = gScrollThumbTop + gScrollThumbHeight;
	var eventY = event.y;
	var delta = 0;
	
	if (gDebug > 0)
		alert("scrollChannelClick - event.y: " + event.y + ", gScrollThumb: [" + gScrollThumbTop + "," + bottomOfThumb +"]");
	if( eventY > bottomOfThumb )
		delta = 4;
	else if ( eventY < topOfThumb )
		delta = -4;
	scrollLineUpDown(delta);
}

function mouseWheelMoved(event)
{
	if (applicationMode == kFindMode) {
		computeScrollParameters();
		var delta = -event.wheelDelta/75;
		scrollLineUpDown(delta);
	}
}

var gIgnoreMouseMoves = true;

function mouseDownScrollbar (event)
{
	document.addEventListener("mousemove", mouseMoveScrollbar, true);
	document.addEventListener("mouseup", mouseUpScrollbar, true);
	
	gStartY = event.y - kWindowToViewOffset;
	gScrollThumb = event.currentTarget;
	gScrollThumbHeight = parseInt(document.getElementById('scroll-mid').style.height);
	gScrollThumbTop = parseInt(document.defaultView.getComputedStyle(gScrollThumb,'').getPropertyValue('top'));
	gNumberOfScrollablePixels = kBottomOfScrollbar - gScrollThumbHeight - kTopOfScrollbar - kWindowToViewOffset;
	event.stopPropagation();
	event.preventDefault();
}

function mouseMoveScrollbar (event)
{
	var yy = event.y-kWindowToViewOffset;
	var deltaY = yy - gStartY;
	
	var newPosition = gScrollThumbTop +deltaY;
	
	if (newPosition < kTopOfScrollbar)
		newPosition = kTopOfScrollbar;
	else if ((newPosition + gScrollThumbHeight+kWindowToViewOffset) > kBottomOfScrollbar)
		newPosition = kBottomOfScrollbar - gScrollThumbHeight - kWindowToViewOffset;

	gScrollThumb.style.top = newPosition + "px";
	
	scrollColumn (newPosition-kTopOfScrollbar, gScrollThumb.id);
	
	event.stopPropagation();
	event.preventDefault();	
}

function mouseUpScrollbar (event)
{
	document.removeEventListener("mousemove", mouseMoveScrollbar, true);
	document.removeEventListener("mouseup", mouseUpScrollbar, true);

	gScrollThumbHeight = 0;
	gScrollThumbTop = 0;
	gScrollThumb  = 0;
	gStartY = -1;
	
	event.stopPropagation();
	event.preventDefault();	
}

function heightOfElement(element)
{
	var topRow = element[0];
	var botRow = element[1];
	var topRowHeight = parseInt(document.defaultView.getComputedStyle(topRow,'').getPropertyValue('height'));
	var botRowHeight = parseInt(document.defaultView.getComputedStyle(botRow,'').getPropertyValue('height'));
	var elementHeight = topRowHeight+botRowHeight+5;	// 5 to cover the gaps
	return elementHeight;
}

function scrollColumn (position, thumb_id)
{
	var viewOffset = position * ((gDocumentHeight - gViewHeight) / gNumberOfScrollablePixels);
	var element = document.getElementById('view');
	
	var firstElement = searchResults[0];
	if (firstElement != null) {
		// the following quantizes the view offset as a multiple of elementHeight
		var elementHeight = heightOfElement(firstElement);
		viewOffset = -Math.floor(viewOffset/elementHeight)*elementHeight;
	} else {
		viewOffset = -Math.ceil(viewOffset);
	}
	
	element.style.top = viewOffset + "px";
}

document.addEventListener("keypress", keyPressed, true);

function keyPressed(e) {
    if ( applicationMode == kFindMode ) {
		var oldSelection = 	parseInt(currentSelection.getAttribute("row"));
		var newSelection;
		switch (e.charCode)
		{
			case 63232: // up
				newSelection = oldSelection - 1;
				break;
			case 63233: // down
				newSelection = oldSelection + 1;
				break;
		}
		if ((newSelection != null) && (newSelection < searchResults.length) && (newSelection >= 0)) {
			var view = document.getElementById('view');
			if (view.hasChildNodes()) {
				var newRow = view.childNodes.item(newSelection);
				clickOnRow(null,newRow);
				var elementHeight = heightOfElement(searchResults[0]);
				var oldTop = parseInt(view.style.top);
				var offset = -newSelection*elementHeight;
				var delta = oldTop-offset;
				var viewOffset;
				if (delta < 0) {
					viewOffset = offset;
				} else if (delta > 3*elementHeight) {
					viewOffset = offset+3*elementHeight;
				}
				if (viewOffset != null) {
					view.style.top = viewOffset;
					gScrollThumbHeight = parseInt(document.getElementById('scroll-mid').style.height);
					gNumberOfScrollablePixels = kBottomOfScrollbar - gScrollThumbHeight - kTopOfScrollbar - kWindowToViewOffset;
					gScrollThumb = document.getElementById('scroll');
					var position = ( delta < 0 	? newSelection*gNumberOfScrollablePixels/(searchResults.length-1)
												: (newSelection-3)*gNumberOfScrollablePixels/(searchResults.length-4)
									);
					gScrollThumb.style.top = (position + kTopOfScrollbar) + "px";
				}
				e.stopPropagation();
				e.preventDefault();
			}
		}
	}
}

function limit_3 (a, b, c) {
    return a < b ? b : (a > c ? c : a);
}

function nextFloat (from, to, ease) {
    return from + (to - from) * ease;
}
var flipAnimation = {duration:0, starttime:0, to:1.0, now:0.0, from:0.0, element:null, timer:null};
var flipShown = false;

function flipAnimate()
{
	var T;
	var ease;
	var time = (new Date).getTime();
		
	
	T = limit_3(time-flipAnimation.starttime, 0, flipAnimation.duration);
	
	if (T >= flipAnimation.duration)
	{
		clearInterval (flipAnimation.timer);
		flipAnimation.timer = null;
		flipAnimation.now = flipAnimation.to;
	}
	else
	{
		ease = 0.5 - (0.5 * Math.cos(Math.PI * T / flipAnimation.duration));
		flipAnimation.now = nextFloat (flipAnimation.from, flipAnimation.to, ease);
	}
	
	flipAnimation.element.style.opacity = flipAnimation.now;
}

function mousemove (event)
{
	if (!flipShown)
	{
		// fade in the flip widget
		if (flipAnimation.timer != null)
		{
			clearInterval (flipAnimation.timer);
			flipAnimation.timer  = null;
		}
		
		var starttime = (new Date).getTime() - 13; // set it back one frame
		
		flipAnimation.duration = 500;
		flipAnimation.starttime = starttime;
		flipAnimation.element = document.getElementById ('flip');
		flipAnimation.timer = setInterval ("flipAnimate();", 13);
		flipAnimation.from = flipAnimation.now;
		flipAnimation.to = 1.0;
		flipAnimate();
		flipShown = true;
	}
}

function mouseexit (event)
{
	if (flipShown)
	{
		// fade in the flip widget
		if (flipAnimation.timer != null)
		{
			clearInterval (flipAnimation.timer);
			flipAnimation.timer  = null;
		}
		
		var starttime = (new Date).getTime() - 13; // set it back one frame
		
		flipAnimation.duration = 500;
		flipAnimation.starttime = starttime;
		flipAnimation.element = document.getElementById ('flip');
		flipAnimation.timer = setInterval ("flipAnimate();", 13);
		flipAnimation.from = flipAnimation.now;
		flipAnimation.to = 0.0;
		flipAnimate();
		flipShown = false;
	}
}

function dismissComboBoxes()
{
    gAirlineCombo.dismissPopup();
    gDepartCombo.dismissPopup();
    gArriveCombo.dismissPopup();
}

function onshow ()
{
    if (applicationMode != kStartMode) {
   	 	setupUpdates(false);
    }
}

function onreceiverequest(request)
{
	var validRequest = false;
	
	if(request)
		validRequest = setGlobalsFromRequest(request);

	//if we found an airline code and flight number then track it!
	if(validRequest)
	{
		gTrackingFlight = {	
			airlineCode: trackingAirlineCode,
			flightNumber: trackingFlightNo,
			departCity: gCurrentDepartCity,
			arriveCity: gCurrentArriveCity,
		}
		trackFlight(gTrackingFlight);	
	}
}

function cancelAllTimers()
{
	if (timerInterval != null) {
		clearInterval(timerInterval);
		timerInterval = null;
	}
//	stopServerAccessAnimation();
	if ((flipAnimation != null) && (flipAnimation.timer != null)) {
		clearInterval (flipAnimation.timer);
		flipAnimation.timer = null;
	}


}

function onhide () {
	cancelAllTimers();
	dismissComboBoxes();
}

function ondragstart() {
	dismissComboBoxes();
}

function onremove () {
	if (window.widget) {
		widget.setPreferenceForKey (null, createkey("departCity"));
		widget.setPreferenceForKey (null, createkey("arriveCity"));
		widget.setPreferenceForKey (null, createkey("airlineCode"));
		widget.setPreferenceForKey (null, createkey("flightNumber"));
		dismissComboBoxes();
	}
}

function showbackside(event)
{
	var back = document.getElementById("behind");
	var front = document.getElementById("front");
	
	if (window.widget)
		widget.prepareForTransition("ToBack");
	
	back.style.display="block";
		
	front.style.visibility="hidden";
	
	if (window.widget)
		setTimeout ('widget.performTransition();', 0);

}

function donePressed(event)
{
	var back = document.getElementById("behind");
	var front = document.getElementById("front");
	if (window.widget)
		widget.prepareForTransition("ToFront");

	back.style.display="none";
	front.style.visibility="visible";
	if (window.widget)
		setTimeout("widget.performTransition();", 0);
}

function backsideLogoClicked()
{
	window.widget.openURL('http://www.flytecomm.com');
}



if (window.widget)
{
    widget.onhide = onhide;
	widget.onshow = onshow;
	widget.onremove = onremove;
	widget.ondragstart = ondragstart;
	widget.onreceiverequest = onreceiverequest;
	window.onmousewheel = mouseWheelMoved;
	window.onblur = dismissComboBoxes
}

function clickOnProvider(event)
{
    widget.openURL("http://www.flytecomm.com");
}

function todayDateString () {
	var today = new Date();
	return zeroPad(today.getDay()) + zeroPad(today.getMonth()+1) + today.getFullYear();	
}

function zeroPad(number) { return (number < 10) ? "0" + number.toString() : number; }


function shrinkToFit(element, desiredWidth, startFontSize)
{
    var elementWidth = 99999;
    var changed = false;
    var fontSize = 99;

	element.style.fontSize = startFontSize + "px"; //need to set in JS for style.fontSize to return value below. 

    while (elementWidth > desiredWidth && fontSize > 5) {

        var computedStyle = document.defaultView.getComputedStyle(element,'');
        fontSize = parseFloat(element.style.fontSize);
        elementWidth = parseInt(computedStyle.getPropertyValue("width"));

        if (elementWidth > desiredWidth)
        {
			element.style.fontSize = (fontSize - 0.1).toString() + "px";
		
			computedStyle = document.defaultView.getComputedStyle(element,'');
			elementWidth = parseInt(computedStyle.getPropertyValue("width"));
			
			changed = true;
		}
		else
		{
			break;
		}
		
    }
    
    return changed;
}
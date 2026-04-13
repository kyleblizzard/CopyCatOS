/*
Copyright © 2005-2007, Apple Inc.  All rights reserved.
NOTE:  Use of this source code is subject to the terms of the Software
License Agreement for Mac OS X, which accompanies the code.  Your use
of this source code signifies your agreement to such license terms and
conditions.  Except as expressly granted in the Software License Agreement
for Mac OS X, no other copyright, patent, or other intellectual property
license or right is granted, either expressly or by implication, by Apple.
*/
var isPlaying = false;
var hasSong = false;
var volume = 0;
var isMouseDown = false;
var currentPosition = 0;
var duration = 0;
var sliderWidth = 130;
var knobLeftmost = 114;
var draggingSlider = false;
var running = false;
var backside = false;
var gLastPlaylist = 0; //  default to "Library"
var gPlaylistChanged = false;
var shuffleState = false;
var repeatState = "off";
var playButtonImage = "";

function getLocalizedString (key)
{
	try {
		var string = localizedStrings[key];
		if (null != string)
			return string;
		else
			return key;
	} catch (ex) {}

	return key;
}

function setPlayButtonImage(src)
{
	if (playButtonImage != src)
	{
		playButtonImage = src;
		document.getElementById("pl").src = src;
	}
}

function mouseMove(e) {
	if (draggingSlider) {
		iTunes.setPlayPosition(((e.pageX-knobLeftmost)/sliderWidth) * duration);
		var dk = document.getElementById("dk");
		dk.style.pixelLeft = e.pageX;
		
		if (dk.style.pixelLeft > knobLeftmost + sliderWidth)
			dk.style.pixelLeft = knobLeftmost + sliderWidth;
		else if (dk.style.pixelLeft < knobLeftmost)
			dk.style.pixelLeft = knobLeftmost;
	}
}

function sliderMouseUp (e)
{
	document.removeEventListener("mousemove", mouseMove, true);
	document.removeEventListener("mouseup", sliderMouseUp, true);
}


function mouseOut(e, tag) {
	switch (tag) {
		case "dk":
			draggingSlider = false;
			break;
	}
	e.stopPropagation();
}

function mouseDown(e, tag) {
	switch(tag) {
		case "ds":
			if (duration)
				iTunes.setPlayPosition((e.layerX/sliderWidth) * duration);
			break;
		case "dk":
			if (duration) {
				iTunes.setPlayPosition(((e.pageX-knobLeftmost)/sliderWidth) * duration);
				var dk = document.getElementById("dk");
				dk.style.pixelLeft = e.pageX;
				draggingSlider = true;
				document.addEventListener("mousemove", mouseMove, true);
				document.addEventListener("mouseup", sliderMouseUp, true);
			}
			break;
		case "prev":
			var fb = document.getElementById("fb");
			fb.src = "Images/dback.png";
			break;
		case "next":
			var fb = document.getElementById("fb");
			fb.src = "Images/dforward.png";
			break;
		case "pl":
			setPlayButtonImage((isPlaying || !hasSong) ? "Images/dpause.png" : "Images/dplay.png");
			break;
		default:
			break;
	}
	
	isMouseDown = true;
	
	e.stopPropagation();
	e.preventDefault();
}

function mouseUp(e, tag) {
	switch(tag) {
		case "prev":
			iTunes.previous();
			var fb = document.getElementById("fb");
			fb.src = "Images/forwardBack.png";
			break;
		case "next":
			iTunes.next();
			var fb = document.getElementById("fb");
			fb.src = "Images/forwardBack.png";
			break;
		case "pl":
			if (gPlaylistChanged)
				iTunes.playPlaylist();
			else
				iTunes.playPause();
			setPlayButtonImage((isPlaying || !hasSong) ? "Images/play.png" : "Images/pause.png");
			break;
		default:
			break;
	}
	
	isMouseDown = false;
}

function go_to_tunes_mousedown (e)
{
	e.stopPropagation();
	e.preventDefault();

}

function go_to_tunes ()
{
	if (window.widget) {
		widget.openApplication ("com.apple.iTunes");
	}
}

function updatePosition() {
	var dk = document.getElementById("dk");
	
	if ((duration != 0)) {
		var xPos = (currentPosition/duration) * sliderWidth;
		if (xPos > sliderWidth)
			xPos = sliderWidth;
			
		dk.style.pixelLeft = knobLeftmost + xPos;
	} else {
		
		dk.style.pixelLeft = knobLeftmost;
	}
}

function updateDisplay() {
	if (window.iTunes) {
		var pos = "0:00";
		var title;
		var e;
		
		running = iTunes.update();
		
		if (running) {
			isPlaying = iTunes.isPlaying();
			
			duration = iTunes.duration();
			currentPosition = iTunes.playerPosition();
			mins = Math.floor(currentPosition / 60);
			secs = currentPosition % 60;
			pos = mins + ":" + ((secs < 10) ? "0" : "") + secs;
					
			title = iTunes.currentTitle();
			if (title == "" || !title)
			{
				title = getLocalizedString("No song.");
				hasSong = false;	
			}
			else
			{
				hasSong = true;
			}
				
		} else {
			title = getLocalizedString("iTunes not running.");
			isPlaying = false;
		}
		
		if (!isMouseDown)
			setPlayButtonImage(isPlaying ? "Images/pause.png" : "Images/play.png");
	
		updatePosition();
		
		e = document.getElementById("timeDisplay");
		if (e.innerText != pos)
			e.innerText = pos;
	
		e = document.getElementById("titleDisplay");
		if (e.innerText != title)
			e.innerText = title;
	}
}

var ourTimer = null;

function pageloaded() {
	//Make info button
	new AppleInfoButton(document.getElementById('info'), document.getElementById('bodyTag'), "black", "black", showbackside);
	
	
	// set the control regions here
	ourTimer = setInterval("updateDisplay();", 1000);
	var dl = document.getElementById("dimple");
	dl.style.pixelTop = 7; dl.style.pixelLeft = 54;

	if (window.widget)
	{
		gLastPlaylist = widget.preferenceForKey(createkey("playlistsave"));
		if (gLastPlaylist == null)
			gLastPlaylist = 0;
		if (iTunes) iTunes.setCurrentPlaylist(gLastPlaylist);
	}
}

if (window.widget) // check to see if the widget object exists
{
	widget.onhide = onhide;
	widget.onshow = onshow;
}


function onshow ()
{
	if (ourTimer == null)
	{
		updateDisplay(); // reload we just shown
		ourTimer = setInterval("updateDisplay()", 1000);
	}

	if (running)
		updatePlaylists();
}


function onhide ()
{
	if (ourTimer != null)
	{
		// we were hidden clear the timer.
		clearInterval(ourTimer);
		ourTimer = null;
	}
}

function createkey(key)
{
	return widget.identifier + "-" + key;
}

var draggingVolume = false;

function ignoreEvent(e) {
	e.stopPropagation();
	e.preventDefault();
}

function checkVolumeMouseDown(id, e) {
	if (backside) return;
	if (!running) return;
	
	var x = e.clientX - 58;
	var y = e.clientY - 52;
	if (Math.sqrt(x*x + y*y) > 45) return;
	
	beginVolumeChange(e);
	document.addEventListener("mousemove", trackVolumeChange, true);
	document.addEventListener("mouseup", trackVolumeChangeUp, true);
	e.stopPropagation();
	e.preventDefault();
}

var lastDAngle;
var lastAngle;
function beginVolumeChange(e) {
	var slider = document.getElementById("ds");
	var knob = document.getElementById("dk");
	var time = document.getElementById("timeDisplay");
	var volEmpty = document.getElementById("volEmpty");
	var volFilled = document.getElementById("volFilled");
	var sh = document.getElementById("shuffle");
	var rp = document.getElementById("repeat");
	
	slider.style.visibility = "hidden";
	knob.style.visibility = "hidden";
	time.style.visibility = "hidden";
	sh.style.visibility = "hidden";
	rp.style.visibility = "hidden";
	volEmpty.style.visibility = "visible";
	volFilled.style.visibility = "visible";
		
	volume = iTunes.volume();
	
	volFilled.style.clip = "rect(0,"+(volume*1.2)+",15,0)";
	
	draggingVolume = true;
	
	var dl = document.getElementById("dimple");
	lastDAngle = Math.atan2(dl.style.pixelTop - 49, dl.style.pixelLeft - 53);
	lastAngle = Math.atan2(e.pageY-50, e.pageX-50) + Math.PI; // make angle 0-2pi
}

function endVolumeChange() {
	var slider = document.getElementById("ds");
	var knob = document.getElementById("dk");
	var time = document.getElementById("timeDisplay");
	var volEmpty = document.getElementById("volEmpty");
	var volFilled = document.getElementById("volFilled");
	var sh = document.getElementById("shuffle");
	var rp = document.getElementById("repeat");
	
	slider.style.visibility = "visible";
	knob.style.visibility = "visible";
	time.style.visibility = "visible";
	sh.style.visibility = "visible";
	rp.style.visibility = "visible";
	volEmpty.style.visibility = "hidden";
	volFilled.style.visibility = "hidden";

	draggingVolume = false;
}

var lastPoint = {x:0, y:0}
var lastWasClockwise = false;
var ignoreCount = 0;

function trackVolumeChange(e) {
	var volFilled = document.getElementById("volFilled");

	// This is a bogus event that is returned just before mouse-up
	if ((e.pageX == -1) && (e.pageY == 115)) {
		return;
	}
	var angle;
	var x = e.pageX-50;
	var y = e.pageY-50;
	angle = Math.atan2(y, x) + Math.PI;
	var range = Math.sqrt(x*x + y*y);
	var threshold = range > 50? .3: 0;
	if ( (Math.abs(angle - lastAngle) <= threshold) &&
		 (ignoreCount < 3) )
	{
		lastPoint.x = e.pageX;
		lastPoint.y = e.pageY;
		ignoreCount++;
		return;
	}
	ignoreCount = 0;
	
	var dl = document.getElementById("dimple");
	var dAngle = angle - lastAngle;
	if (dAngle > Math.PI) dAngle = angle - (lastAngle + (Math.PI * 2.0));
	if (dAngle < -Math.PI) dAngle = (angle + (Math.PI * 2.0)) - lastAngle;
	lastDAngle += dAngle;
	dl.style.pixelTop = 49 + Math.sin(lastDAngle)*41.5;
	dl.style.pixelLeft = 53 + Math.cos(lastDAngle)*41.0;
	
	var localVolume = volume * 1.2;
	dVolume = (120.0 * dAngle)/Math.PI;
	if (Math.abs(dVolume) < 2) dVolume = (dVolume < 0)? -1: 2;
	localVolume += dVolume;
	if (localVolume >= 120)
		localVolume = 120;
	else if (localVolume <= 0)
		localVolume = 0;
		
	volFilled.style.clip = "rect(0,"+parseInt(localVolume)+",15,0)";
	volume = parseInt(localVolume/1.2);
	iTunes.setVolume(volume);
	
	lastAngle = angle;
	lastPoint.x = e.pageX;
	lastPoint.y = e.pageY;
		
	e.stopPropagation();
	e.preventDefault();
}

function trackVolumeChangeUp(e) {
	document.removeEventListener("mousemove", trackVolumeChange, true);
	document.removeEventListener("mouseup", trackVolumeChangeUp, true);
	endVolumeChange();
	e.stopPropagation();
	e.preventDefault();
}

function showbackside(event)
{
	var front = document.getElementById("FrontSide");
	var back = document.getElementById("behind");
	
	if (window.widget)
		widget.prepareForTransition("ToBack");
	
	front.style.display="none";
	back.style.display="block";
	
	if (running)
		updatePlaylists();
	if (window.widget)		
		setTimeout ('widget.performTransition();', 0);	

	backside = true;
}


function doneMouseUp(event)
{
	var front = document.getElementById("FrontSide");
	var back = document.getElementById("behind");
	
	if (window.widget)
		widget.prepareForTransition("ToFront");
	
	front.style.display="block";
	back.style.display="none";
	if (window.widget)
		setTimeout ('widget.performTransition();', 0);
	backside = false;

	if (gPlaylistChanged && iTunes) {
		if (isPlaying)
			iTunes.playPlaylist();
		shuffleState = iTunes.shuffle();
		document.getElementById("shuffle").src = shuffleState? "Images/shuffle_on.png" :"Images/shuffle_off.png";
		repeatState = iTunes.songRepeat();
		document.getElementById("repeat").src = "Images/repeat_" + repeatState + ".png";		
	}
}

function updatePlaylists() {
	if (iTunes) {
		if (iTunes.fetchPlaylists()) {
			
			var popup = document.getElementById("playlist-popup");
			
			popup.options.length = 0; // clear the current list
			
			var lCount = iTunes.playlistCount();
			for (var list = 0; list < lCount; list++) {
				var listName = iTunes.playlistNameForIndex(list);
				if (listName != "missing value") {
					var newList = new Option(listName);
					popup.options[popup.options.length] = newList;
				}
			}
			
			var myIndex = parseInt(iTunes.currentPlaylistIndex());
			if (myIndex < 0) // iTune is not playing, so doesn't have a playlist
				myIndex = gLastPlaylist;
				
			popup.options[myIndex].selected = true;
			gPlaylistChanged = false;
		}
		
		shuffleState = iTunes.shuffle();
		repeatState = iTunes.songRepeat();
	}
	
	document.getElementById("shuffle").src = shuffleState? "Images/shuffle_on.png" :"Images/shuffle_off.png";
	document.getElementById("repeat").src = "Images/repeat_" + repeatState + ".png";
}

function playlistchanged(elem)
{	
	var list = elem.selectedIndex;
	
	if (running) {
		gLastPlaylist = list;
		if (iTunes) iTunes.setCurrentPlaylist(gLastPlaylist);
		gPlaylistChanged = true;
		
		if (window.widget)
		{
			widget.setPreferenceForKey(gLastPlaylist, createkey("playlistsave"));
		}
	}
}

function toggleShuffle(event) {
	if (running) {
		shuffleState = shuffleState? 0: 1;
		if (iTunes) {
			iTunes.setShuffle(shuffleState? 1: 0);
			shuffleState = iTunes.shuffle();
		}
		document.getElementById("shuffle").src = shuffleState? "Images/shuffle_on.png": "Images/shuffle_off.png";
	}
}

function incrementRepeat(event) {
	if (running) {
		var value = 0;
		
		if (repeatState == "off") {
			repeatState = "all";
			value = 2;
		}
		else if (repeatState == "all") {
			repeatState = "one";
			value = 1;
		}
		else if (repeatState == "one") {
			repeatState = "off";
			value = 0;
		}
		if (iTunes) {
			iTunes.setRepeat(value);
			repeateState = iTunes.songRepeat();
		}
		document.getElementById("repeat").src = "Images/repeat_" + repeatState + ".png";
	}
}

function debug(msg) {
	if (!debug.box) {
		debug.box = document.createElement("div");
		debug.box.setAttribute("style", "background-color: white; " +
										"font-family: monospace; " +
										"border: solid black 3px; " +
										"padding: 10px;");
		document.body.appendChild(debug.box);
		debug.box.innerHTML = "<h1 style='text-align:center'>Debug Output</h1>";
	}
	
	var p = document.createElement("p");
	p.appendChild(document.createTextNode(msg));
	debug.box.appendChild(p);
}

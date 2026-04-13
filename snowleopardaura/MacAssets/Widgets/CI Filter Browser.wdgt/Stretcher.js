/*

Copyright _ 2005, Apple Computer, Inc.  All rights reserved.
NOTE:  Use of this source code is subject to the terms of the Software
License Agreement for Mac OS X, which accompanies the code.  Your use
of this source code signifies your agreement to such license terms and
conditions.  Except as expressly granted in the Software License Agreement
for Mac OS X, no other copyright, patent, or other intellectual property
license or right is granted, either expressly or by implication, by Apple.

*/ 

/*
 ***************************************************************
 * <Stretcher object definition.  Stretches a div up and down> *
 ***************************************************************
 */
 

/*
 * Stretcher constructor.  Parameters:
 *
 * -- element: The element to stretch
 * -- doneNotification: A callback (if no callback is needed, pass null)
 *
 */
function Stretcher (element, stretchHeight, doneNotification) {
	this.element = element;

	this.startTime = 0;
	this.timer = null;
	
	// min and max position can be changed to alter the stretched/shrunk sizes.
	var computedStyle = document.defaultView.getComputedStyle(this.element, "");
	this.minPosition = parseInt(computedStyle.getPropertyValue("height"));
	this.maxPosition = this.minPosition + stretchHeight;
	//DEBUG('Stretcher min: ' + this.minPosition);
	this.positionFrom = this.minPosition;
	this.positionNow = this.minPosition;
	this.widthNow = parseInt(computedStyle.getPropertyValue("width"));
	this.positionTo = 0;
	
	this.isCollapsed = false;
	this.stretching = false;
	
	this.stretch = Stretcher_stretch;
	this.tick = Stretcher_tick;
	this.doneNotification = doneNotification;
}

/*
 * This should only be called via a Stretcher instance, i.e. "instance.stretch(event)"
 * Calling Stretcher_stretch() directly will result in "this" evaluating to the window
 * object, and the function will fail.  Parameters:
 * 
 * -- event: the mouse click that starts everything off (from an onclick handler).
 *		We check for the shift key to do a slo-mo stretch.
 */
function Stretcher_stretch (event) {
	if (this.stretching) return;
	
	this.positionFrom = this.positionNow;
	
	var resizeTo = this.isCollapsed ? this.minPosition : this.maxPosition;

	this.positionTo = parseInt(resizeTo); // lots of hard coding, yum...
	
	var multiplier = (event.shiftKey ? 10 : 1); // enable slo-mo
	var timeNow = (new Date).getTime();
	this.stretchTime = 250 * multiplier;
	this.startTime = timeNow - 13; // set it back one frame.
	
	// We need to store this in a local variable so the timer does not lose scope
	// when invoking tick.
	var localThis = this;
	this.stretching = true;
	this.timer = setInterval (function() { localThis.tick(); }, 13);
	this.tick();
}
		
/*
 * Tick does all the incremental resize work.
 * This function is very similar to the tick() function in the Fader sample.
 */
function Stretcher_tick () {
	var T;
	var ease;
	var time  = (new Date).getTime();
	var yLoc;
	var frame;
		
	T = limit_3(time-this.startTime, 0, this.stretchTime);
	ease = 0.5 - (0.5 * Math.cos(Math.PI * T / this.stretchTime));

	if (T >= this.stretchTime) {
		yLoc = this.positionTo;
		clearInterval (this.timer);
		this.timer = null;
		this.isCollapsed = !this.isCollapsed;
		if (this.doneNotification) {
			// call after the last frame is drawn
			var localThis = this;
			setTimeout (function() { localThis.doneNotification(); }, 0);
		}
		this.stretching = false;
	} else {
		yLoc = computeNextFloat(this.positionFrom, this.positionTo, ease);
	}
	// convert to a integer, not sure if this is the best way
	this.positionNow = parseInt(yLoc);
	this.element.style.height = this.positionNow + "px";
	window.resizeTo(this.widthNow, yLoc);
}

/*
 * Support functions for the stretch animation
 */
function limit_3 (a, b, c) {
    return a < b ? b : (a > c ? c : a);
}

function computeNextFloat (from, to, ease) {
    return from + (to - from) * ease;
}
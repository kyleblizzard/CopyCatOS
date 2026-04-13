/*

Copyright _ 2005, Apple Computer, Inc.  All rights reserved.
NOTE:  Use of this source code is subject to the terms of the Software
License Agreement for Mac OS X, which accompanies the code.  Your use
of this source code signifies your agreement to such license terms and
conditions.  Except as expressly granted in the Software License Agreement
for Mac OS X, no other copyright, patent, or other intellectual property
license or right is granted, either expressly or by implication, by Apple.

*/

var	currentScroller;
var tracking					= true;	// is the mouse down in the scroll track


/*
 ********************************
 *	Utility / Math functions	*
 ********************************
 */
 
function getProportion (viewheight, documentheight) {
	if (documentheight <= viewheight)
		return 0;
	else
		return viewheight/documentheight;
}

// Given the position of the thumb, tell us what the content top should be.
// This is the key value that allows us to thumb-scroll.
function pagePositionForThumbPosition (thumbPosition) {
	return -(thumbPosition - SCROLLBAR_TOP) * ((this.currentContentHeight - this.viewHeight) / this.numberOfScrollablePixels);
}

// Given the position of the page, tell us where the thumb should be.
// This is the key value that allows us to track-scroll.
function thumbPositionForPagePosition (pagePosition) {
	//DEBUG("thumbPositionForPagePosition : " + pagePosition);
	return -(pagePosition / ((this.currentContentHeight - this.viewHeight) / this.numberOfScrollablePixels)) + SCROLLBAR_TOP;
}


function scrollContent(newThumbPosition) {
		//DEBUG("scrollContent: newPositionIn=" + newThumbPosition);

	// Correct if we're going to clip above the top or below the bottom
	if (newThumbPosition < SCROLLBAR_TOP) {
			//DEBUG("sc: thumb too high (" + newThumbPosition + ")");
		newThumbPosition = SCROLLBAR_TOP;
	} else if ((newThumbPosition + this.thumbHeight) > this.scrollBarHeight) {
			//DEBUG("sc: thumb too low (" + newThumbPosition + ")");
		newThumbPosition = this.scrollBarHeight - this.thumbHeight;
	}
		
		//DEBUG("scrollContent: newPosition=" + newThumbPosition);
	this.scrollThumb.style.top = newThumbPosition + 'px';
	
		//DEBUG("calculating delta");
	this.currentContentTop = this.pagePositionForThumbPosition(newThumbPosition);
		//DEBUG("scrollContent: thumbTop=" + this.scrollThumb.style.top + " this.currentContentTop is " + this.currentContentTop);
	this.currentContent.style.top = this.currentContentTop + 'px';
}

// Hide the thumb and track, but keep the parent scrollbar DIV around to preserve formatting
function hideScrollBar() {
	this.scrollTrack.style.display = 'none';
	this.scrollThumb.style.display = 'none';
}

function showScrollBar() {
	this.scrollTrack.style.display = 'block';
	this.scrollThumb.style.display = 'block';
}


/*
 * Scroller constructor.  Parameters:
 *
 * -- element: The element to stretch
 *
 */
 
Scroller.prototype.thumbPositionForPagePosition = thumbPositionForPagePosition;
Scroller.prototype.pagePositionForThumbPosition = pagePositionForThumbPosition;
Scroller.prototype.scrollContent = scrollContent;
Scroller.prototype.hideScrollBar = hideScrollBar;
Scroller.prototype.showScrollBar = showScrollBar;
Scroller.prototype.pageUp = pageUp;
Scroller.prototype.pageDown = pageDown;
Scroller.prototype.mouseMoveTrack = mouseMoveTrack;	
Scroller.prototype.mouseOutTrack = mouseOutTrack;	
Scroller.prototype.mouseOverTrack = mouseOverTrack;	
Scroller.prototype.trackTimer = null;	// for extended mousedowns in the scroll track

function Scroller (contentDiv, inScrollBar, inScrollThumb, inScrollTrack) 
{
/*
 ****************************************
 * Objects used by the Scroller	*
 ****************************************
 */	
	//this.currentContentStyle;	// style object of the currentContent

	this.currentContent = contentDiv;										// currently-visible DIV.  Necessary for the Scroller to be shared across DIVs
	this.scrollBar = inScrollBar;											// Parent scrollbar DIV.  Contains the track and thumb
	this.scrollThumb = inScrollThumb;										// Scroller's thumb control
	this.scrollTrack = inScrollTrack;										// Scroller's base/track


	if(this.currentContent == null) {
		this.hideScrollBar();
	}

	this.currentContent.style.display = 'block';
	this.currentContentStyle = document.defaultView.getComputedStyle(this.currentContent,'');
		//DEBUG("cast: cct=" + this.currentContentStyle.getPropertyValue('top'));

/*
 ********************************************
 *  Dimensions used by the Scroller	*
 ********************************************
 */
	this.currentContentTop = parseInt(this.currentContentStyle.getPropertyValue('top'));
	this.currentContentHeight = parseInt(this.currentContentStyle.getPropertyValue('height'));// height of currently-visible content DIV
	this.viewHeight = parseFloat (document.defaultView.getComputedStyle(this.currentContent.parentNode, '').getPropertyValue('height'));// height of the parent (overflow:hidden) view
		//DEBUG("cast: this.viewHeight=" + this.viewHeight);
	this.scrollBarHeight = parseInt(document.defaultView.getComputedStyle(this.scrollBar, '').getPropertyValue('height'));// height of our scrollbar.  TBD
		//DEBUG("cast: this.currentContentHeight=" + this.currentContentHeight + " top=" + this.currentContentTop + " this.scrollBarHeight=" + this.scrollBarHeight);

		
	this.trackMouseY = 0;						// mouse location in the scroll track
	this.thumbHeight = 0;						// height of the thumb control
	this.thumbStartY					= -1;	// point where we started scrolling with the thumb
	this.scrollThumbStartPos			= -1;	// thumb's 'top' value when we started scrolling
	this.numberOfScrollablePixels = 0;			// for calculating thumb size/position and content position ("page number")
	
	var percent = getProportion (this.viewHeight, this.currentContentHeight);
	//DEBUG("cast: percent=" + percent);
	
	// hide the scrollbar if all the content is showing.  Determined by the calculated scrollbar height and position.
	if (percent == 0) {
			//DEBUG("cast: 0% thumbHeight.  Hiding scrollbar");
		this.currentContent.style.top = '0px';
		this.hideScrollBar();
	} else {
		// Position the thumb according to where the content is currently scrolled.
		// This is necessary for sharing the same scrollbar between multiple content
		// panes that will likely be at different scroll positions.
		this.thumbHeight = Math.max(Math.round(this.scrollBarHeight * percent), SCROLL_THUMB_HEIGHT);
		var thumbTop = this.thumbPositionForPagePosition(this.currentContentTop);
		
		this.scrollThumb.style.height = this.thumbHeight + 'px';
		this.scrollThumb.style.top = thumbTop;
		
		this.numberOfScrollablePixels = this.scrollBarHeight - this.thumbHeight - SCROLLBAR_TOP;
			//DEBUG("cast: new thumb height=" + this.scrollThumb.style.height + "thumbTop" + thumbTop);
		
		// This is a safeguard so the content matches the new thumb position.  Necessary for live-resizing to work.
		this.scrollContent(thumbTop);
		
		this.showScrollBar();
	}

}this.thumbPositionForPagePosition



/*
 ********************************************************
 * Constants.  Hardcoded to match respective CSS values	*
 ********************************************************
 */
	var SCROLLBAR_TOP		= -1;
	var SCROLL_THUMB_HEIGHT	= 27;
	
	// CSS element names of critical DIVs.  Abstracted for easy customization.
	var TRACK_TOP_DIV_NAME		= 'categoryScrollTrackTop';	// Top edge of scroller track
	var TRACK_MID_DIV_NAME		= 'categoryScrollTrackMid';	// variable-size center of scroller track
	
	var PAGE_SKIP_PAUSE	= 150; // time (ms) between page jumps when holding the mouse down in the track.
	
// Calculate the height of the views and make the thumb proportional.
// If a single scroller is being shared across multiple divs (as in this sample),
// this function must be called whenever the divs swap, to recalibrate the scrollbar.



/*
 ********************************
 *	Thumb Scrolling Functions	*
 ********************************
 */

// This mouseDown is presumably the start of a thumb drag (scroll) action.
function mouseDownScrollThumb (scroller, event) {
//DEBUG("mouseDownScrollThumb");
	// We add these listeners and remove them later; they're only useful while there is mouse activity
	// on the thumb.  This is necessary because there is no mousedrag event in JavaScript.
	currentScroller = scroller;
	document.addEventListener("mousemove", mouseMoveScrollThumb, true);
	document.addEventListener("mouseup", mouseUpScrollThumb, true);
	
	scroller.thumbStartY = event.y;

	scroller.scrollThumbStartPos = parseInt(document.defaultView.getComputedStyle(scroller.scrollThumb,'').getPropertyValue('top'));

		//DEBUG("mdThumbHeight:" + scroller.thumbHeight + " this.thumbStartY=" + scroller.thumbStartY);
}

// At this point we are dragging the scrollThumb.  We know this because the mousemove listener is only installed
// after a mousedown.
function mouseMoveScrollThumb (event) {
//DEBUG("mouseMoveScrollThumb");
	var deltaY = event.y - currentScroller.thumbStartY;
	
	var newPosition = currentScroller.scrollThumbStartPos + deltaY;
		//DEBUG("mmst: event.y=" + event.y + " this.thumbStartY=" + currentScroller.thumbStartY + " thumbStart=" + currentScroller.scrollThumbStartPos);
	currentScroller.scrollContent(newPosition);
}


function mouseUpScrollThumb (event) {
		//DEBUG("must: eventY=" + event.y);
	// After mouseup, these events are just noise. Remove them; they'll be re-added on the next mouseDown
	document.removeEventListener("mousemove", mouseMoveScrollThumb, true);
	document.removeEventListener("mouseup", mouseUpScrollThumb, true);
	
	// reset the starting position
	currentScroller.thumbStartY = -1;
}

/*
 ********************************
 *	Track Scrolling Functions	*
 ********************************
 */

function mouseDownTrack (scroller, event) {
//DEBUG("mouseDownTrack");
	updateTrackMouseY(scroller, event);
	
	scroller.scrollTrack.addEventListener("mousemove", scroller.mouseMoveTrack, false);
	scroller.scrollTrack.addEventListener("mouseover", scroller.mouseOverTrack, false);
	scroller.scrollTrack.addEventListener("mouseout", scroller.mouseOutTrack, false);
	
	// This is our handling for clicks in the track.
	var thumbTop = document.defaultView.getComputedStyle(scroller.scrollThumb,'').getPropertyValue('top');
		//DEBUG("trackClick: mouseY=" + scroller.trackMouseY + " scrollThumbY=" + thumbTop);
	currentScroller = scroller;
	if (scroller.trackMouseY > parseInt(thumbTop)) {
			//DEBUG("click BELOW thumb ");
		scroller.pageDown();
		scroller.trackTimer = setInterval("pageDown();", PAGE_SKIP_PAUSE);
	} else {
			//DEBUG("click ABOVE thumb");
		scroller.pageUp();
		scroller.trackTimer = setInterval("pageUp();", PAGE_SKIP_PAUSE);
	}
	
	//DEBUG("mdt: newPosition=" + scroller.trackMouseY);
}

function mouseMoveTrack(event) {
	// If the mouse moved while being held down, update the location so we 
	// stop the track-scrolling in the right place.
	updateTrackMouseY(this, event);
}

function mouseOutTrack(event) {
	// When the mouse moves out while pressed, we turn track-scrolling off.
	// The timer keeps firing, but pageUp/pageDown exits based on this value
	tracking = false;
}

function mouseOverTrack(event) {
	// The timer is still firing, but pageUp/pageDown are waiting for the mouse to
	// return to the track.  This will resume track-scrolling.
	tracking = true;
}

function mouseUpTrack(scroller, event) {
	// stop track-scrolling
	clearInterval(scroller.trackTimer);
	
	// After mouseup, these events are just noise. Remove them; they'll be re-added on the next mouseDown
	scroller.scrollTrack.removeEventListener("mousemove", scroller.mouseMoveTrack, false);
	scroller.scrollTrack.removeEventListener("mouseover", scroller.mouseMoveTrack, false);
	scroller.scrollTrack.removeEventListener("mouseout", scroller.mouseMoveTrack, false);
}

// correct the coordinates for the sourceEvent so they properly match the source component
// **YOU MAY NEED TO UPDATE THIS FUNCTION** depending on how deeply the scrollbar div is nested
function updateTrackMouseY (scroller, event) {
	//DEBUG("utmY: source=" + event.toElement.className + event.toElement.parentElement.children[0].className + " offsetY=" + event.offsetY + " layerY=" + event.layerY + " offsetTop=" + event.toElement.offsetTop);
	
	if (event.toElement.className == 'scrollTrackMid') {
		// source is the ctr component of the track; offset by the top component.
		var topHeight = document.defaultView.getComputedStyle(event.toElement.parentElement.children[0]).getPropertyValue('height');
		scroller.trackMouseY = event.offsetY + parseInt(topHeight);
		//DEBUG("utmY: click in mid, topHeight=" + topHeight);
	} else if (event.toElement.className == 'scrollTrackBot') {
		// source is the bottom component of the track; offset by the top and the middle.
		var midHeight = document.defaultView.getComputedStyle(event.toElement.parentElement.children[1]).getPropertyValue('height');
		var topHeight = document.defaultView.getComputedStyle(event.toElement.parentElement.children[0]).getPropertyValue('height');
		scroller.trackMouseY = event.offsetY + parseInt(midHeight) + parseInt(topHeight);
		//DEBUG("utmY: click in bottom, offsetY=" + event.offsetY + " offsetTop=" + event.toElement.offsetTop + " mid+top=" + (parseInt(midHeight) + parseInt(topHeight)));
	} else {
		//DEBUG("utmY: click in top, offsetY=" + event.offsetY + " offsetTop=" + event.toElement.offsetTop + " parentTop=" + event.toElement.parentNode.offsetTop);
		// source is the top of the track
		scroller.trackMouseY = event.offsetY - (event.toElement.offsetTop + event.toElement.parentNode.offsetTop);
	}	
	
	//DEBUG("utmY: trackMouseY" + scroller.trackMouseY);
}

/*
 ********************************************************************
 * pageUp/pageDown													*
 * Used by track-scrolling code, but can be called independently	*
 ********************************************************************
 */ 

// Reposition the content one page (this.viewHeight) upwards.  Prevent out-of-bounds values.
// Remember that the content top becomes increasingly NEGATIVE (moves upwards) as we scroll down.	
function pageDown() {
	if (!tracking) return;
	// calculate the last page.  This is equal to the content's full height, less one this.viewHeight
	// Again, the value is negative because that's how far offset the content would need to be.
	currentScroller.currentContentTop = parseInt(currentScroller.currentContentStyle.getPropertyValue('top'));
	var lastPageY = -(currentScroller.currentContentHeight - currentScroller.viewHeight);
	// calculate the next page from the content's current position.
	var nextPageY = currentScroller.currentContentTop - currentScroller.viewHeight;
		//DEBUG("pageDown: currentScroller.currentContent.lastPageY=" + lastPageY + " nextPageY=" + nextPageY);
	currentScroller.currentContentTop = Math.max(lastPageY, nextPageY);
	currentScroller.currentContent.style.top = currentScroller.currentContentTop + 'px';

	// reposition the scroll thumb based on the new page position.
	var newThumbTop = currentScroller.thumbPositionForPagePosition(currentScroller.currentContentTop);
	var thumbBottom = newThumbTop + parseInt(currentScroller.scrollThumb.style.height);
	currentScroller.scrollThumb.style.top = newThumbTop;

		//DEBUG("pageDown: currentScroller.currentContentTop=" + currentScroller.currentContentTop + " currentScroller.currentContentHeight=" + currentScroller.currentContentHeight + " newThumbTop=" + newThumbTop + " thumbBottom=" + thumbBottom);

	if (currentScroller.trackMouseY < thumbBottom) {
		// the thumb has met the mouse; time to stop track-scrolling.
		clearInterval(currentScroller.trackTimer);
	}
}

// very similar to pageDown, with some values negated to move the content in a different direction.
function pageUp() {
	if (!tracking) return;
	currentScroller.currentContentTop = parseInt(currentScroller.currentContentStyle.getPropertyValue('top'));
	var firstPageY = 0;
	var nextPageY = currentScroller.currentContentTop + currentScroller.viewHeight;
	currentScroller.currentContentTop = Math.min(firstPageY, nextPageY)
	currentScroller.currentContent.style.top = currentScroller.currentContentTop + 'px';
	
	var newThumbTop = currentScroller.thumbPositionForPagePosition(currentScroller.currentContentTop);
		//DEBUG("pageUp: contentTop=" + currentScroller.currentContentTop + " newThumbTop=" + newThumbTop);// + " thumbBottom=" + thumbBottom);
	currentScroller.scrollThumb.style.top = newThumbTop;

	if (currentScroller.trackMouseY > newThumbTop) {
		clearInterval(currentScroller.trackTimer);
	}
}


/*
 ****************************
 *	END SCROLLER FUNCTIONS	*
 ****************************
 */


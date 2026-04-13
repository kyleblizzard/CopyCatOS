/*
Copyright (c) 2005, Apple Computer, Inc.  All rights reserved.
NOTE: Use of this source code is subject to the terms of the Software
License Agreement for Mac OS X, which accompanies the code.  Your use
of this source code signifies your agreement to such license terms and
conditions.  Except as expressly granted in the Software License Agreement
for Mac OS X, no other copyright, patent, or other intellectual property
license or right is granted, either expressly or by implication, by Apple.
*/
/**
 *	@class 	A spinning, animated, indeterminate ProgressIndicator.
 *  		Usage:
 *			<pre>var c = document.getElementById("myCanvasElement"); // get ref to canvas element on the page
var prog = new AppleProgressIndicator(c); // pass canvas ref to the constructor
prog.setRGB(0,255,255); //make the progress indicator aqua
prog.startAnimation(); // begin the animation
...
prog.stopAnimation(); // stop animation later
</pre>
 *	@constructor
 *	@param {HTMLCanvasElement} el A canvas element on the current page
 *	@param {Boolean} displayWhenStopped use <tt>true</tt> to cause this indicator to be visible while not animated
 */
function AppleProgressIndicator(el, displayWhenStopped) {
	/** @private */
	this._canvas = el;
	/** @private */
	this._displayWhenStopped = displayWhenStopped;
	/** @private */
	this._ctx = this._canvas.getContext("2d");

	/** @private */
	this._canvasWidth = parseInt(this._canvas.width);
	/** @private */
	this._canvasHeight = parseInt(this._canvas.height);

	/** @private */
	this._isAnimating = false;
	/** @private */
	this._barWidth = this._canvasWidth/13;
	/** @private */
	this._barHeight = this._canvasWidth/2;

	/** @private */
	this._numBars = 12;
	/** @private */
	this._barCornerRadius = this._canvasWidth/25;
	/** @private */
	this._angle = Math.PI*2/this._numBars;

	/** @private */
	this._refreshDuration = 30;

	/** @private */
	this._ctx.translate(this._canvasWidth/2, this._canvasHeight/2);
	/** @private */
	this._r = 0;
	/** @private */
	this._g = 0;
	/** @private */
	this._b = 0;
}

/**
 *	By default the progress indicator bars are black (0,0,0) whith a transparent background.
 *	@param {Number} r	Red value for color of bars 0 - 255
 *	@param {Number} g	Green value for color of bars 0 - 255
 *	@param {Number} b	Blue value for color of bars 0 - 255
 *	@returns {void}
 */
AppleProgressIndicator.prototype.setRGB = function(r, g, b) {
	this._r = parseInt(r);
	this._g = parseInt(g);
	this._b = parseInt(b);
};

/**
 *	starts the progress animation
 *	@returns {void}
 */
AppleProgressIndicator.prototype.startAnimation = function() {
	if (this._isAnimating) {
		return;
	}
	this._isAnimating = true;
	if (!this._displayWhenStopped) {
		this._canvas.style.display = "";
	}
	var _self = this;
	this._intervalId = setInterval(function(){_self._paint()}, this._refreshDuration);
};

/**
 *	stop the progress animation
 *	@returns {void}
 */
AppleProgressIndicator.prototype.stopAnimation = function() {
	this._isAnimating = false;
	clearInterval(this._intervalId);
	this._intervalId = null;
	if (!this._displayWhenStopped) {
		this._canvas.style.display = "none";
	}
};

/**
 *	@returns {String} <tt>[object AppleProgressIndicator]</tt>
 */
AppleProgressIndicator.prototype.toString = function() {
	return "[object AppleProgressIndicator]";
};

/**
 *	@private
 *	@returns {void}
 */
AppleProgressIndicator.prototype._paint = function() {
	this._ctx.clearRect(-this._canvasWidth/2, -this._canvasHeight/2, this._canvasWidth, this._canvasHeight);

	this._ctx.rotate(this._angle);

	var alpha = .5;
	var fillStyle = "rgba(" + this._r + ", " + this._g + ", " + this._b + ", ";
	for (var i = 0; i < this._numBars; i++) {
		this._ctx.rotate(this._angle);
		this._ctx.fillStyle = fillStyle + alpha + ")";
		
		var x = -(this._barWidth/2);
		var y = this._canvasWidth/5;
		//ctx.fillRect (x, y, barWidth, barHeight-y);

		this._ctx.beginPath();
		this._ctx.moveTo(x, y+this._barCornerRadius);
		this._ctx.arcTo(x, this._barHeight, x+this._barWidth, this._barHeight, this._barCornerRadius);
		this._ctx.arcTo(x+this._barWidth, this._barHeight, x+this._barWidth, y, this._barCornerRadius);
		this._ctx.arcTo(x+this._barWidth, y, x, y, this._barCornerRadius);
		this._ctx.arcTo(x, y, x, this._barHeight, this._barCornerRadius);
		this._ctx.fill();

		alpha -= .45/this._numBars;
	}
};
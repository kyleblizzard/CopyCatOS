/*
Copyright 2005, Apple Computer, Inc.  All rights reserved.
NOTE:  Use of this source code is subject to the terms of the Software
License Agreement for Mac OS X, which accompanies the code.  Your use
of this source code signifies your agreement to such license terms and
conditions.  Except as expressly granted in the Software License Agreement
for Mac OS X, no other copyright, patent, or other intellectual property
license or right is granted, either expressly or by implication, by Apple.
*/

window.onfocus = function () {
	document.getElementById('search-input').focus();
}

function search (input)
{
	var value = input.value;
	if (value.length > 0)
	{
		value = encodeURIComponent (value);
		var url = "http://www.google.com/search?q=" + value + "&ie=UTF-8&oe=UTF-8";
		if (window.widget)
			widget.openURL (url);
	}
}

function keydown (event, input)
{
	if (event.keyCode == 13) // enter or return
	{
		search (input);
	}
}
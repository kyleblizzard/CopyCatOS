/*
Copyright 2005, Apple Computer, Inc.  All rights reserved.
NOTE:  Use of this source code is subject to the terms of the Software
License Agreement for Mac OS X, which accompanies the code.  Your use
of this source code signifies your agreement to such license terms and
conditions.  Except as expressly granted in the Software License Agreement
for Mac OS X, no other copyright, patent, or other intellectual property
license or right is granted, either expressly or by implication, by Apple.
*/

var gStates = new Array;
/*gStates["ALABAMA"] = "Alabama";
gStates["ALASKA"] = "Alaska";
gStates["ARIZONA"] = "Arizona";
gStates["ARKANSAS"] = "Arkansas";
gStates["CALIFORNIA"] = "California";
gStates["COLORADO"] = "Colorado";
gStates["CONNECTICUT"] = "Connecticut";
gStates["DELAWARE"] = "Delaware";
gStates["DISTRICT OF COLUMBIA"] = "District of Columbia";
gStates["FLORIDA"] = "Florida";
gStates["GEORGIA"] = "Georgia";
gStates["HAWAII"] = "Hawaii";
gStates["IDAHO"] = "Idaho";
gStates["ILLINOIS"] = "Illinois";
gStates["INDIANA"] = "Indiana";
gStates["IOWA"] = "Iowa";
gStates["KANSAS"] = "Kansas";
gStates["KENTUCKY"] = "Kentucky";
gStates["LOUISIANA"] = "Louisiana";
gStates["MAINE"] = "Maine";
gStates["MARYLAND"] = "Maryland";
gStates["MASSACHUSETTS"] = "Massachusetts";
gStates["MICHIGAN"] = "Michigan";
gStates["MINNESOTA"] = "Minnesota";
gStates["MISSISSIPPI"] = "Mississippi";
gStates["MISSOURI"] = "Missouri";
gStates["MONTANA"] = "Montana";
gStates["NEBRASKA"] = "Nebraska";
gStates["NEVADA"] = "Nevada";
gStates["NEW HAMPSHIRE"] = "New Hampshire";
gStates["NEW JERSEY"] = "New Jersey";
gStates["NEW MEXICO"] = "New Mexico";
gStates["NEW YORK"] = "New York";
gStates["NORTH CAROLINA"] = "North Carolina";
gStates["NORTH DAKOTA"] = "North Dakota";
gStates["OHIO"] = "Ohio";
gStates["OKLAHOMA"] = "Oklahoma";
gStates["OREGON"] = "Oregon";
gStates["PENNSYLVANIA"] = "Pennsylvania";
gStates["RHODE ISLAND"] = "Rhode Island";
gStates["SOUTH CAROLINA"] = "South Carolina";
gStates["SOUTH DAKOTA"] = "South Dakota";
gStates["TENNESSEE"] = "Tennessee";
gStates["TEXAS"] = "Texas";
gStates["UTAH"] = "Utah";
gStates["VERMONT"] = "Vermont";
gStates["VIRGINIA"] = "Virginia";
gStates["WASHINGTON"] = "Washington";
gStates["WEST VIRGINIA"] = "West Virginia";
gStates["WISCONSIN"] = "Wisconsin";
gStates["WYOMING"] = "Wyoming";*/
gStates["ALABAMA"] = "AL";
gStates["ALASKA"] = "AK";
gStates["ARIZONA"] = "AZ";
gStates["ARKANSAS"] = "AR";
gStates["CALIFORNIA"] = "CA";
gStates["COLORADO"] = "CO";
gStates["CONNECTICUT"] = "CT";
gStates["DELAWARE"] = "DE";
gStates["DISTRICT OF COLUMBIA"] = "DC";
gStates["FLORIDA"] = "FL";
gStates["GEORGIA"] = "GA";
gStates["HAWAII"] = "HI";
gStates["IDAHO"] = "ID";
gStates["ILLINOIS"] = "IL";
gStates["INDIANA"] = "IN";
gStates["IOWA"] = "IA";
gStates["KANSAS"] = "KA";
gStates["KENTUCKY"] = "KY";
gStates["LOUISIANA"] = "LA";
gStates["MAINE"] = "ME";
gStates["MARYLAND"] = "MD";
gStates["MASSACHUSETTS"] = "MA";
gStates["MICHIGAN"] = "MI";
gStates["MINNESOTA"] = "MN";
gStates["MISSISSIPPI"] = "MS";
gStates["MISSOURI"] = "MO";
gStates["MONTANA"] = "MT";
gStates["NEBRASKA"] = "NE";
gStates["NEVADA"] = "NV";
gStates["NEW HAMPSHIRE"] = "NH";
gStates["NEW JERSEY"] = "NJ";
gStates["NEW MEXICO"] = "NM";
gStates["NEW YORK"] = "NY";
gStates["NORTH CAROLINA"] = "NC";
gStates["NORTH DAKOTA"] = "ND";
gStates["OHIO"] = "OH";
gStates["OKLAHOMA"] = "OK";
gStates["OREGON"] = "OR";
gStates["PENNSYLVANIA"] = "PA";
gStates["RHODE ISLAND"] = "RI";
gStates["SOUTH CAROLINA"] = "SC";
gStates["SOUTH DAKOTA"] = "SD";
gStates["TENNESSEE"] = "TN";
gStates["TEXAS"] = "TX";
gStates["UTAH"] = "UT";
gStates["VERMONT"] = "VT";
gStates["VIRGINIA"] = "VA";
gStates["WASHINGTON"] = "WA";
gStates["WEST VIRGINIA"] = "WV";
gStates["WISCONSIN"] = "WI";
gStates["WYOMING"] = "WY";
gStates["AL"] = "AL";
gStates["AK"] = "AK";
gStates["AZ"] = "AZ";
gStates["AR"] = "AR";
gStates["CA"] = "CA";
gStates["CO"] = "CO";
gStates["CT"] = "CT";
gStates["DE"] = "DE";
gStates["DC"] = "DC";
gStates["FL"] = "FL";
gStates["GA"] = "GA";
gStates["HI"] = "HI";
gStates["ID"] = "ID";
gStates["IL"] = "IL";
gStates["IN"] = "IN";
gStates["IA"] = "IA";
gStates["KS"] = "KS";
gStates["KY"] = "KY";
gStates["LA"] = "LA";
gStates["ME"] = "ME";
gStates["MD"] = "MD";
gStates["MA"] = "MA";
gStates["MI"] = "MI";
gStates["MN"] = "MN";
gStates["MS"] = "MS";
gStates["MO"] = "MO";
gStates["MT"] = "MT";
gStates["NE"] = "NE";
gStates["NV"] = "NV";
gStates["NH"] = "NH";
gStates["NJ"] = "NJ";
gStates["NM"] = "NM";
gStates["NY"] = "NY";
gStates["NC"] = "NC";
gStates["ND"] = "ND";
gStates["OH"] = "OH";
gStates["OK"] = "OK";
gStates["OR"] = "OR";
gStates["PA"] = "PA";
gStates["RI"] = "RI";
gStates["SC"] = "SC";
gStates["SD"] = "SD";
gStates["TN"] = "TN";
gStates["TX"] = "TX";
gStates["UT"] = "UT";
gStates["VT"] = "VT";
gStates["VA"] = "VA";
gStates["WA"] = "WA";
gStates["WV"] = "WV";
gStates["WI"] = "WI";
gStates["WY"] = "WY";




var gCacheData;				// just the data
var gLineViews;				// html for the line views
var gPhoneBookMainView;		// main view
var gScrollController;		// manage the scrollbar, and tell the main view to scroll

var gSearchWithinOptionsArray = ['10','20','50','100'];
var gItemsPerPageOptions = 		['10','20','30','40'];

var gDefaultSearchWithinRange = 1;
var gDefaultItemsPerPageArrayIndex = 0;

var gDefaultSearchFromAddress = new Array();

var gCustomSearchAddressSet = false;

//var gCurrentSearchFromCity = "";
//var gCurrentSearchFromState = "";
//var gCurrentSearchFromZip = "";

//var constZipIfNoUserLocation = "95014";
//var constStateIfNoUserState = "CA";

var gUpdatedUserAddress = false;

var currentSelection = null;
var currentSelectionIdx = -1;
						
var constMainResizeAnimationDuration = 275;
										
var constPreRollExpandingBoxMinHeight = 0;
var constPreRollExpandingBoxFullHeight = 25;
var constPreRollExpandingBoxAnimationDuration = 50;

var constPreRollBottomHalfMinOverlapPos = 0;
var constPreRollBottomHalfMaxOverlapPos = -29;
var constPreRollOverlapAnimationDuration = 50;

gConstWidgetCompactHeight = 76;
gConstWidgetExpandedTopHeight = 65;
gConstWidgetCollapsedMidHeight = 0;
gConstWidgetExpandedMidHeight = 158;
gConstWidgetResultsViewWidth = 326;
gConstWidgetBottomHeight = 41;
gConstWidgetExpandedHeight = parseInt(gConstWidgetExpandedTopHeight) + parseInt(gConstWidgetExpandedMidHeight) + parseInt(gConstWidgetBottomHeight);

var gConstMouseWheelScrollPixels = 11;
var gConstMouseWheelDeltaDivisor = 75;

var gResizeAnimation = null;

// animation for the flip's alpha fade
var animation = {startTime:0, duration:250, positionFrom:0, positionTo:0, positionNow:0, frameFrom:0, frameTo:0, frameNow:0, timer:null, element:null};

var timerInterval = null;
var flipShown = false;

var currentFrame = -1;
var gTotalFrames = 0;

var kCategoryLengthMax = 60;

// support for Apple parser 
var gLastQuery = null;

function pbAlert( msg )
{
	//alert( msg ); // uncomment for lots of console output
}

// Begin MJP White Pages Additions

var gFocusedField = -1 // -1 = none, 0 = lastname, 1 = firstname, 2 = citystate
var gSettingFlag = false;
var gTabbingFlag = false;
var gValidationFailed = false;

function focusFirstName()
{
	gFocusedField = 1;
	document.getElementById("highlight-right").style.visibility = 'hidden';
	document.getElementById("highlight-left").style.visibility = 'visible';
}	

function focusLastName()
{
	gFocusedField = 0;
	document.getElementById("highlight-left").style.visibility = 'hidden';
	document.getElementById("highlight-right").style.visibility = 'visible';
}

function unfocusFirstName()
{
	if (gFocusedField == 1)
		gFocusedField = -1;
	document.getElementById("highlight-left").style.visibility = 'hidden';
}

function unfocusLastName()
{
	if (gFocusedField == 0)
		gFocusedField = -1;
	document.getElementById("highlight-right").style.visibility = 'hidden';
}

function focusCityState()
{
	gValidationFailed = false;
	gFocusedField = 2;
	document.getElementById("highlight-left").style.visibility = 'hidden';
	document.getElementById("highlight-right").style.visibility = 'hidden';
}

function unfocusCityState()
{
	if (gFocusedField == 2)
		gFocusedField = -1;
	if (!gValidationFailed && document.embeds['citystate-input'].value().length > 0)
	{
		gTabbingFlag = true;	
		cityStateChanged();
	}
	gValidationFailed = false;
}

// This class controls the general UI of the widget, including the scrolling results view
function CPhoneBookMainView()
{
	// data members
	this.fIsCollapsed = true;
	this.fFrontSideShowing = true;
	
	this.fDocumentHeight = 0;
	this.fViewHeight = 0;
	
	// these are the indices that mark the range of the data cache that we are displaying
	this.fDataDisplayStartIndex = -1;
	this.fDataDisplayEndIndex = -1;
	
	// how many results per page...
	this.fDataDisplayResultsPerPage = gItemsPerPageOptions[gDefaultItemsPerPageArrayIndex];
	
	// we may need to query multiple times to get enough results to meet certain requirements
	// this tracks how may results we got so far in a single loop of searches (a loop is one search, with x many queries to the server)
	this.fDataResultsCountOfOneSearchLoop = 0;

	// set by the popup on the backside, how far to search
	this.fSearchWithinRangeValue = gSearchWithinOptionsArray[gDefaultSearchWithinRange];
	
	// function members
	this.getResultsView = function()  { return document.getElementById('resultsView') };
	this.getResultsTableView = function() { return document.getElementById('resultsTableDiv') };
	
	this.getCurrentRangeLimit = function() { return this.fSearchWithinRangeValue };
	
	// these assignments just map function names to their implementations down below
	this.setNoResults = _setNoResults;
	this.reset = _reset;
	//this.setCustomAddress = _setCustomAddress;
	this.preferencesUpdated = _preferencesUpdated;
	this.setDisplayIndicesToNext = _setDisplayIndicesToNext;
	this.setDisplayIndicesToPrevious = _setDisplayIndicesToPrevious;
	this.displayedResultsCount = _displayedResultsCount;
	this.clearDisplay = _clearDisplay;
	this.showHideResultsView = _showHideResultsView;
	this.showHideTitle = _showHideTitle;
	this.showHideArrows = _showHideArrows; // this is really to force refresh
	this.updateViewGeometryInfo = _updateViewGeometryInfo;	
	this.build = _build;
	this.growView = _growView;
	this.updateArrows = _updateArrows;
	this.hasMoreCachedData = _hasMoreCachedData;
	this.getViewToDocRatio = _getViewToDocRatio;
	this.getPixelToDocRatio = _getPixelToDocRatio;
	this.scrollView = _scrollView;

	function _preferencesUpdated()
	{	
		var prefValueChanged = false;
		
		if( this.fDataDisplayStartIndex != -1 ) // don't bother if we're in unset state
		{
			if( parseInt( this.fDataDisplayResultsPerPage ) != gItemsPerPageOptions[gDefaultItemsPerPageArrayIndex] )
			{
				prefValueChanged = true;
				
				this.fDataDisplayResultsPerPage = gItemsPerPageOptions[gDefaultItemsPerPageArrayIndex];
				
				if( ( parseInt( this.fDataDisplayStartIndex ) - parseInt( this.fDataDisplayResultsPerPage ) ) <= 0 )
				{
					this.fDataDisplayStartIndex = 0;
					this.fDataDisplayEndIndex = parseInt( this.fDataDisplayResultsPerPage ) - 1;
				}
				else
				{
					this.fDataDisplayStartIndex =  parseInt( this.fDataDisplayStartIndex ) - parseInt( this.fDataDisplayResultsPerPage );
					this.fDataDisplayEndIndex = parseInt( this.fDataDisplayStartIndex) + parseInt( this.fDataDisplayResultsPerPage ) - 1;
				}
				
				if( parseInt( this.fDataDisplayEndIndex ) > parseInt( gCacheData.getLength() ) )
					this.fDataDisplayEndIndex = parseInt( gCacheData.getLength() );
			}
			
			if( parseInt( this.fSearchWithinRangeValue ) != parseInt( gSearchWithinOptionsArray[gDefaultSearchWithinRange] ))
			{
				prefValueChanged = true;
				this.fSearchWithinRangeValue = parseInt(gSearchWithinOptionsArray[gDefaultSearchWithinRange]);
			}
			
			
		}
		
		return prefValueChanged;
	}

	// no results, so update the UI to show that
	function _setNoResults( reason )
	{	
		// hide results table
		document.getElementById('resultsTableDiv').style.visibility="hidden";
		
		// show title screen
		this.showHideTitle( true );
		
		// display status string for no results
		document.getElementById('titleScreen-label').innerText = getLocalizedString(reason);
		
		// update arrows
		noArrows();
		
		// display results number
		document.getElementById('statusTextDiv').innerText = "";
	}
	
	// upon a new search, need to reset everything
	function _reset()
	{
		this.fDataDisplayStartIndex = -1;
		this.fDataResultsCountOfOneSearchLoop = 0;
		gCacheData.reset();
		this.clearDisplay();
	}
	
	// the display indicies track which range of results in our data cache should be displayed in the main view
	// this function advances to the next range
	function _setDisplayIndicesToNext()
	{
		// is this a new search?  Then the "next" index is the starting index
		if( this.fDataDisplayStartIndex == -1 )
		{
			this.fDataDisplayStartIndex = 0;
			this.fDataDisplayResultsPerPage = gItemsPerPageOptions[gDefaultItemsPerPageArrayIndex];
			this.fDataDisplayEndIndex = ( this.fDataDisplayResultsPerPage  - 1 ); // zero based index
		}
		else // are we advancing past the first page?
		{
			if( parseInt( this.fDataDisplayStartIndex) + parseInt( this.fDataDisplayResultsPerPage) < parseInt(gCacheData.getLength()) )
			{
				this.fDataDisplayStartIndex = parseInt( this.fDataDisplayStartIndex) + parseInt( this.fDataDisplayResultsPerPage );
				
				if( (this.fDataDisplayEndIndex + this.fDataDisplayResultsPerPage) < gCacheData.getLength() )
					this.fDataDisplayEndIndex = parseInt( gCacheData.getLength());
				else
					this.fDataDisplayEndIndex = parseInt( this.fDataDisplayEndIndex ) + parseInt( this.fDataDisplayResultsPerPage );
			}
			else
			{
				//pbAlert( "Can't advance to next index as it appears to be beyond end of cache...");
			}
		}
	}
	
	// the display indicies track which range of results in our data cache should be displayed in the main view
	// this function advances to the previous range
	function _setDisplayIndicesToPrevious()
	{
		// back up by one frame, but don't regress past zero!
		if( (parseInt( this.fDataDisplayStartIndex ) - parseInt( this.fDataDisplayResultsPerPage)) < 0 )
			this.fDataDisplayStartIndex = 0;
		else
			this.fDataDisplayStartIndex = parseInt( this.fDataDisplayStartIndex ) - parseInt( this.fDataDisplayResultsPerPage );
			
		if( ( parseInt( this.fDataDisplayEndIndex ) - parseInt( this.fDataDisplayResultsPerPage )) < parseInt( this.fDataDisplayResultsPerPage - 1) )
			this.fDataDisplayEndIndex = parseInt( parseInt( this.fDataDisplayResultsPerPage - 1));
		else
			this.fDataDisplayEndIndex = parseInt( this.fDataDisplayEndIndex ) - parseInt( this.fDataDisplayResultsPerPage );
	}
	
	function _displayedResultsCount()
	{
		var resultsView = this.getResultsView();
		return resultsView.children.length;
	}
	
	function _clearDisplay()
	{
		var view = this.getResultsView();
		
		// remove the previous results
		while (view.hasChildNodes())
		{
			view.removeChild(view.firstChild);
		}
		
		var resultView = document.getElementById('resultsView');

		resultView.style.height = "auto";
		resultView.style.width = "";
		
		// we don't want to show the ledger lines if collapsed.
		
		// Also, a situation can occur where the widget is collapes and the lines would be shown.
		// if the combo box menu is up and a search is executed, we may start up an
		// animation sequence, which will be delayed until the menu goes away
		// so don't show the ledger lines here if we have a gResizeAnimation in progress
		// and the ledger lines will be shown at the right time when the
		// gResizeAnimation starts progressing again
		if( !this.fIsCollapsed )//&& gResizeAnimation == null )
		{
			document.getElementById('extraTopLedgerLineImg').style.display="block";
			document.getElementById('extraBottomLedgerLineImg').style.display="block";
		}
		
	    document.getElementById('titleScreen-label').innerText = getLocalizedString('White Pages');

		gLineViews.reset();
		gScrollController.updateScrollbarForNewView(0);
	}
	
	function _showHideResultsView( show )
	{
		var tableDiv = this.getResultsTableView();
		
		tableDiv.style.visibility = ( show == true ? "visible" : "hidden" );
	}
	
	function _updateViewGeometryInfo()
	{
		var resultsView = this.getResultsView();
		
		var docStyle = document.defaultView.getComputedStyle(resultsView, '');
		if( docStyle != null )
			this.fDocumentHeight = parseFloat(docStyle.getPropertyValue('height'));
		else
			this.fDocumentHeight = 0;
		
		var viewStyle = document.defaultView.getComputedStyle(resultsView.parentNode.parentNode.parentNode, '');

		if( viewStyle != null )
			this.fViewHeight = parseFloat (viewStyle.getPropertyValue('height'));
		else
			this.fViewHeight = 0;
	}
	
	function _build()
	{	
		var rangeLimit = this.getCurrentRangeLimit();
		
		var linesAddedToView = -1;
		// note that while we attempt to set a range of data, any out of range lines won't be set, so the end index
		// is not indicative the index of the last line in the lines view
		
		linesAddedToView = gLineViews.addData( gCacheData.fAddressData, this.fDataDisplayStartIndex, this.fDataDisplayEndIndex, rangeLimit );

		// This check below fixes a "gotcha" case where the user has originally specified a very large search radius
		// then paged through the results to display a page with results that include very distant "hits", and then user decides to reset the search radius to 
		// a lower number.  When that happens, we need to back up through the cache (which is ordred from lowest to highest range) until we find some results
		// in range, or we reach the start of the cache.
		while( gLineViews.getLength() == 0 && this.fDataDisplayStartIndex > 0 )
		{
	        this.setDisplayIndicesToPrevious();
    	    this.build();
    	    return;
		}
		
		if( gLineViews.getLength() > 0 )
		{
			var linesView = this.getResultsView();
			gLineViews.addLinesToThisView( linesView );

			this.showHideTitle( false );
			this.showHideResultsView( true );
		}
		else
		{
			this.setNoResults(getLocalizedString('No Results'));
		}
		
		this.updateArrows();
		
		this.updateViewGeometryInfo();
		
		updateStatusText( false, this.fDataDisplayStartIndex, parseInt(this.fDataDisplayStartIndex) + gLineViews.getLength() );

		document.getElementById('resultsView').style.top = 0;		
		updateScrollbar();
	}
	
	function _growView(expand, doItSlow)
	{	
		if ( ( this.fIsCollapsed && expand == false) ||
		     (!this.fIsCollapsed && expand == true) )
            return;
		
		// see comments just above CAnimator to better understand the growView animation...
		
		if( gResizeAnimation ) // boundary condition, trying to start animation when some other animation is still running
		{		
			gResizeAnimation.forceEnd(); // tell outstanding animations to end
		}
		
		var bodyMid = document.getElementById("bodyMidDiv");
		
		if( this.fIsCollapsed ) // If we are expanding, we will animate the preroll element...
		{
			var preRollExpandingBoxHeightNow = constPreRollExpandingBoxMinHeight;
			var preRollExpandingBoxStartHeight = constPreRollExpandingBoxMinHeight;
			var preRollExpandingBoxEndHeight = constPreRollExpandingBoxFullHeight;
			
			var expandingBox = document.getElementById("expandAnimationBlock");
			
			gResizeAnimation = new CAnimator( (constPreRollExpandingBoxAnimationDuration) /*duration*/, preRollExpandingBoxHeightNow, preRollExpandingBoxStartHeight /*from*/, 
												 preRollExpandingBoxEndHeight /*to*/, expandingBox/*elem*/, 'height', finishPreRollExpandingBox /*finishcallack*/ );

			bodyMid.style.height = gConstWidgetCollapsedMidHeight; // body must start out collapsed
			
			gPhoneBookMainView.fIsCollapsed = false;
			gResizeAnimation.runAnimation();
		}
		else
		{	
			gResizeAnimation = new CAnimator( constMainResizeAnimationDuration, gConstWidgetExpandedMidHeight, gConstWidgetExpandedMidHeight,
													 gConstWidgetCollapsedMidHeight, bodyMid, 'height', mainAnimationFinished);
			gPhoneBookMainView.fIsCollapsed = true;
			gResizeAnimation.runAnimation();
		}
	}
	
	function _showHideTitle( show )
	{
		if( show == true && this.fFrontSideShowing )
		{
			document.getElementById('titleScreenDiv').style.visibility = "visible";
			document.getElementById('titleScreen-label').style.visibility = "visible";
		}
		else
		{
			document.getElementById('titleScreenDiv').style.visibility = "hidden";
			document.getElementById('titleScreen-label').style.visibility = "hidden";
		}
	}
	
	function _showHideArrows( show )
	{
	    var backArrow = document.getElementById('backArrow');
	    var forwardArrow = document.getElementById('forwardArrow');
	    backArrow.style.visibility = show?'visible':'hidden';
	    forwardArrow.style.visbility = show?'visible':'hidden';	    
	}
	
	function _updateArrows()
	{
		var rangeLimit = this.getCurrentRangeLimit();
		
		// if we have a full page of results in the current view, maybe there is more to come in the cache or to be searched for?
		var enableForwardArrow = gLineViews.getLength() == this.fDataDisplayResultsPerPage;
		
		//If we have not yet exceeded the range limit, maybe there is more
		// data in the cache, or to be searched for?  (note that if not range data is returned by the feed, then fLargestDistance is always zero)
		if( enableForwardArrow && parseFloat( gLineViews.fLargestDistance ) <= parseFloat( rangeLimit ) )
		{
			var distanceOfNextItem = gCacheData.getDistanceOfIndexedItem( parseInt(this.fDataDisplayEndIndex) + 1 );
			if(  ( parseInt( this.fDataDisplayEndIndex ) + 1 ) < ( parseInt( gCacheData.getLength() ) ))
			{	
				// only update the enable status if the data has valid range
				if( distanceOfNextItem > 0 )
					enableForwardArrow = ( parseFloat( distanceOfNextItem ) <= parseFloat( rangeLimit ) );
				else if( distanceOfNextItem == -1 )
					enableForwardArrow = false;
			}
			else
			{
				if( (gCacheData.fLargestDistance != 0 ) && parseFloat( gCacheData.fLargestDistance ) > parseFloat( rangeLimit ) )
				{
					enableForwardArrow = false;
				}
				else if( distanceOfNextItem == -1 )
					enableForwardArrow = false;
			}
		}
			
		var enableBackwardArrow = ( this.fDataDisplayStartIndex > 0 );
		 // update the arrow controls
        updateArrows( enableBackwardArrow, enableForwardArrow );
	}
	
	function _hasMoreCachedData()
	{
		var virtualCacheSize = parseInt( gCacheData.getLength() ); // we try to keep 10 more lines of cache than we need, so we can "look ahead"
		var lastItemDisplayed = parseInt( this.fDataDisplayEndIndex ) + 1;
		
		// so we only have more cache data if there is
		if( parseInt( lastItemDisplayed + ( parseInt( this.fDataDisplayResultsPerPage ) ) ) <  ( parseInt( virtualCacheSize ) -1 ) )
		{
			return true;
		}
		return false;
	}
	
	function _getViewToDocRatio()
	{
		if( this.fDocumentHeight <= this.fViewHeight )
			return 0.0;
		else
			return parseFloat(this.fViewHeight/this.fDocumentHeight);
	}
	
	function _getPixelToDocRatio( pixelCount )
	{
		if( this.fDocumentHeight <= this.fViewHeight )
			return 0.0;
		else
			return parseFloat(parseInt(pixelCount)/this.fDocumentHeight);
	}
	
	function _scrollView( relativePos )
	{	
		var element = this.getResultsView();
		
		// a single result has a height = docHeight / number of results
		
		var singleResultLineHeight = parseInt( parseInt( this.fDocumentHeight ) / gLineViews.fLineViewsArray.length );
		
		// the bottom of the scrollable area is docHeight - ( number of results in a single view * single result Height );
		
		var scrollableArea = parseInt( this.fDocumentHeight ) - parseInt( singleResultLineHeight * 3);
		
		newPosition = relativePos * scrollableArea;
		
		newPosition = -Math.round (newPosition);
		element.style.top = newPosition + "px";		
	}
}


// This class maintains arrays of HTML data used to display
// the page views of addresses generated by a search.
function CLineViews()
{
	// each item in the array is a page of data
	// each page has n number of result lines returned by a search
	// where n is an upper limit on results per page or less.
	this.fLineViewsArray = new Array();
	this.fLargestDistance = 0;
	
	// functions defined below
	this.reset = _reset;
	this.addData = _addData;
	this.addLinesToThisView = _addLinesToThisView;
	this.getLength = function() { return this.fLineViewsArray.length };
	
	function _reset()
	{
		while(this.fLineViewsArray.length)
		{
			this.fLineViewsArray.pop();
		}

		this.fLargestDistance = 0;
	}
	
	// add query results to the array.
	function _addData( addrData, startIdx, endIdx, rangeLimit )
	{
//pbAlert("_addData");
		this.fLargestDistance = 0;
        for( var i = startIdx; ( i <= endIdx && i < addrData.length ); i++ )
        {
            var singleResult = addrData[i];
            
			if( parseFloat( singleResult.distance)  > parseFloat( this.fLargestDistance)  )
				this.fLargestDistance = singleResult.distance;
	            	
            if( parseFloat(singleResult.distance) > parseFloat( rangeLimit ) )
            {
                break;
            }
            
            var oneAddressLineView = document.createElement("div");
        
            var nameSpan = document.createElement("span");
            nameSpan.setAttribute("class", "topRow");
			nameSpan.setAttribute("id", "personName");
            //pbAlert("addData: person name '"+singleResult.firstname+"' '"+singleResult.lastname+"'");
			nameSpan.innerText = singleResult.firstname+" "+singleResult.lastname;
         
			nameSpan.setAttribute("profileID", singleResult.encrypted_id );
				
            nameSpan.onclick = function( event )
            {
            	if( window.widget )
            		widget.openURL( getProfilePageURL(this.getAttribute("profileID") ) );
            }
            
            oneAddressLineView.appendChild(nameSpan);
    		
            var phoneSpan = document.createElement("span");
            phoneSpan.setAttribute("class", "topRow");
            phoneSpan.setAttribute("id", "personPhone");
            phoneSpan.setAttribute("AB_phone", singleResult.phone );
            phoneSpan.onclick = function (event)
            {
                //if (window.WhitePagesPlugin) 
                //	WhitePagesPlugin.showLargeType(this.getAttribute("AB_phone"));
                
                if ( document.embeds['citystate-input'] != null && this.getAttribute("AB_phone").length > 0)
                {
                    document.embeds['citystate-input'].getSubPlugin().showLargeType(this.getAttribute("AB_phone"));
                }
            };
            phoneSpan.innerText = singleResult.phone;
            
            oneAddressLineView.appendChild(phoneSpan);
    
    		/*
    		
    		distance information is not used at this time
    		if this line is uncommented, the CSS layout will need to readjust...
    		
            var distanceSpan = document.createElement("span");
            distanceSpan.setAttribute("class", "topRow");
            distanceSpan.setAttribute("id", "businessDistance");
            distanceSpan.innerText = singleResult.distance + getLocalizedString(' Miles');
            oneAddressLineView.appendChild(distanceSpan);
            
			*/
			
			var addressDiv = document.createElement("div");
			addressDiv.setAttribute("class", "addressDiv");
			
			var addToAddressBookDiv = document.createElement("button");
			addToAddressBookDiv.innerHTML = "<span>" 
				+ getLocalizedString("Add %@ to AddressBook.").replace("%@", singleResult.firstname + singleResult.lastname) 
				+ "</span>";
			addToAddressBookDiv.setAttribute("class", "addToAddressBookDiv");
			addToAddressBookDiv.setAttribute("AB_firstname", singleResult.firstname );
			addToAddressBookDiv.setAttribute("AB_lastname", singleResult.lastname );
			addToAddressBookDiv.setAttribute("AB_address", singleResult.address );
			addToAddressBookDiv.setAttribute("AB_city", singleResult.city );
			addToAddressBookDiv.setAttribute("AB_state", singleResult.state );
			addToAddressBookDiv.setAttribute("AB_zip", singleResult.zip );
			addToAddressBookDiv.setAttribute("AB_phone", singleResult.phone );
			
			addToAddressBookDiv.onclick = function( event )
			{
				if( window.widget )
				{
					this.style.background="url(Images/add_onclick.png) no-repeat center center";
					
					gLineViews.addToAddressBookTarget = this;
					
					var confirmDialog = document.getElementById("popupDialog");
					var busName = document.getElementById('popupDialogLine1');
					var confirmText = document.getElementById('popupDialogLine2');
					
					busName.innerText = this.getAttribute("AB_firstname") + " " + this.getAttribute("AB_lastname");
					confirmText.innerText="Add this entry to Address Book?"
					//show the confirm dialog
					confirmDialog.style.visibility = "visible";
				}
			}
			
			addToAddressBookDiv.onmousedown = function( event )
			{
				this.style.background="url(Images/add_onclick.png) no-repeat center center";
			}
			
			addToAddressBookDiv.onmouseover = function(event)
			{
				if( window.widget )
				{
					this.style.background="url(Images/add_onmouseover.png) no-repeat center center";
				}
			}
			
			addToAddressBookDiv.onmouseout = function(event)
			{
				if( window.widget )
				{
					this.style.background="url(Images/add.png) no-repeat center center";
				}
			}
			
			addToAddressBookDiv.style.background="url(Images/add.png) no-repeat center center";

			oneAddressLineView.appendChild(addToAddressBookDiv);
			
			// this span contains the two address lines.  It is required so either line can be clicked and
			// call the click handler
			var addressSpansContainer = document.createElement("div");
			addressSpansContainer.setAttribute( "class", "businessAddressContainer" );
			addressSpansContainer.setAttribute( "id", "businessAddressContainer" );
			
			// example query
			
			//http://www.mapquest.com/maps/map.adp?country=us&address=1+Infinite+Loop&city=Cupertino&state=CA&zip=95014
			
			var mapURL = "http://www.daplus.us/showmap.aspx?";
			
			// try to delete suite / apt # info at end of address, otherwise MapQuest will not work.
			mapURL += "name=";
			mapURL += singleResult.firstname.replace(/\s+/g, "+");
			mapURL += "+";
			mapURL += singleResult.lastname.replace(/\s+/g, "+");
			if (singleResult.address.length > 0)
			{
				mapURL += "&address=";
				var tmpAddr = singleResult.address.replace(/(\s*#.*$)/, "");
				mapURL += tmpAddr.replace(/\s+/g, "+");
			}
			mapURL += "&city=";
			mapURL += singleResult.city.replace(/\s+/g, "+");
			mapURL += "&state=";
			mapURL += singleResult.state.replace(/\s+/g, "+");
			if (singleResult.zip.length > 0)
			{
				mapURL += "&zip=";
				mapURL += singleResult.zip.replace(/\s+/g, "+");
			}
			
			//mapURL += "&Partner=400119";
			mapURL += "&Partner=applewp";
			//pbAlert("map url "+mapURL);
			
			addressSpansContainer.setAttribute("maplink", mapURL );
			addressSpansContainer.onclick = function(event)
			{
				if( window.widget )
					widget.openURL(this.getAttribute("maplink"));
			}
			
			var addressSpanLine1 = document.createElement("div");
			addressSpanLine1.setAttribute("class", "businessAddressClass" );
			addressSpanLine1.setAttribute("id", "businessAddressLine1id");
			
            // put address line 1 in the clickable container
            addressSpansContainer.appendChild(addressSpanLine1);
            
			var addressSpanLine2 = document.createElement("div");
			addressSpanLine2.setAttribute("class", "businessAddressClass" );
			addressSpanLine2.setAttribute("id", "businessAddressLine2id");
			
			// if street address is present, build the whole address
			if ( singleResult.address.length > 0 )
			{
				addressSpanLine1.innerText = singleResult.address;
				addressSpanLine2.innerText = singleResult.city + ", " + singleResult.state + " " + singleResult.zip;
			}
			else // if no street, then put city, state, zip on the first line.  (if this is blank too, that's ok)
			{
				addressSpanLine1.innerText = singleResult.city + ", " + singleResult.state + " " + singleResult.zip;
			}
            
            // put address line 2 in the clickable container
            addressSpansContainer.appendChild(addressSpanLine2);
            
            // put the container in the result row view
            oneAddressLineView.appendChild(addressSpansContainer);

            oneAddressLineView.setAttribute("class","findResult");
            
            this.fLineViewsArray.push( oneAddressLineView );
		}
	}
	
			
	function _addLinesToThisView( linesView )
	{
		// add the new results
		var lineCount = this.getLength();
		
		var resultView = document.getElementById('resultsView');
		
		if( lineCount > 0 && lineCount <= 3)
		{
			resultView.style.height = gConstWidgetExpandedMidHeight;
			resultView.style.width = gConstWidgetResultsViewWidth;
			
			// when expanded, and there is no scroll bar, the ledger line should go all the way to the right
			document.getElementById('extraTopLedgerLineImg').style.display="block";
			document.getElementById('extraBottomLedgerLineImg').style.display="block";
		}
		else
		{
			document.getElementById('extraTopLedgerLineImg').style.display="none";
			document.getElementById('extraBottomLedgerLineImg').style.display="none";
		}
		
		var anAddress;
		var thisAddressArray = this.fLineViewsArray;
		for( var k=0; k < lineCount; k++ )
		{
			anAddress = thisAddressArray[k] ;
			if( anAddress )
				resultView.appendChild( anAddress );
		}
	}
}

function CCacheData()
{
	this.fAddressData = new Array();
	
	this.fLargestDistance = 0;
	this.fDataAddCount = 0;
	
	this.addData = _addData;
	
	this.getLength = function() { return this.fAddressData.length };
	this.getDistanceOfIndexedItem = _getDistanceOfIndexedItem;
	this.getHighestIndexWithinRange = _getHighestIndexWithinRange;
	this.reset = _reset;

	function _addData( dataMeta )
	{
		var data = dataMeta.businesses;
		var idx = this.fAddressData.length;
		var anAddress;
		var dataIdx;
		for( dataIdx = 0; dataIdx < data.length; dataIdx++ )
		{
			anAddress = data[dataIdx];
	
            // keep track of largest distance found
            if( anAddress.distance )
            {
            	if( parseFloat(anAddress.distance) > parseFloat( this.fLargestDistance ) )
            		this.fLargestDistance = anAddress.distance;
			}
			
			this.fAddressData[idx] = anAddress;
			idx++;
		}
		this.fDataAddCount = this.fDataAddCount+1;
		return ( parseInt( dataIdx ) );
	}
	
	function _getDistanceOfIndexedItem( idx )
	{
		if( parseInt( idx ) < parseInt( this.getLength() ) )
		{
			var anAddress = this.fAddressData[idx];
	
            var dist = parseFloat( anAddress.distance );
            if( !dist )
            {
            	dist = 0.0;
            }
            return dist;
		}
		return -1.0;
	}
	
	function _getHighestIndexWithinRange( range )
	{
		var idx = (this.getLength() - 1);
		// starting from end of cache, back down thru the items looking for the range.
		for( idx; idx > 0; idx-- )
			if( parseFloat( this.fAddressData[ idx ].distance ) <= parseFloat( range ) )
				return parseInt( idx );

		return -1; // range not found
	}
	
	function _reset()
	{
		while( (this.fAddressData).length > 0 )
		{
			this.fAddressData.pop();
		}
		this.fContainsAddressesBeyondUserRangePreference = false;
		this.fLargestDistance = 0;
		this.fDataAddCount = 0;
	}
}

function CScroll( scrollbarHeight, channelAbsoluteVerticalOffset )
{
	this.currentY = 0;
	this.thumbHeight = 0;
	this.thumbTop = 0;
	this.scrollablePixels = 0;
	this.scrollChannelDelta = 0;
	
	this.kScrollbarBottom = scrollbarHeight;
	this.kScrollbarHeight = parseInt(scrollbarHeight);
	
	// for event handling, we sometimes need to know what the absolute 
	// offset of the scrollbar channel is from the top of the widget
	this.kChannelOffset = parseInt(channelAbsoluteVerticalOffset);
	
	this.isScrolling = false;
	
	this.getThumbBottom = _getThumbBottom;
	
	this.getThumb = function() { return document.getElementById('scrollthumb'); };
	
	this.moveThumbTo = _moveThumbTo;
	this.scrollDelta = _scrollDelta;
	this.mouseUp = _mouseUp;
	this.mouseDown = _mouseDown;
	this.mouseDrag = _mouseDrag;
	this.mouseWheelMoved = _mouseWheelMoved;
	this.channelClick = _channelClick;
	
	this.updateThumbPosition = _updateThumbPosition
	this.updateScrollbarForNewView = _updateScrollbarForNewView;
	
	//this.debugDump = _debugDump;
	
	function _getThumbBottom()
	{
		return parseInt( parseInt( this.thumbTop ) + parseInt( this.thumbHeight ) + 6);
	}
	
	function _moveThumbTo( thumbPosition )
	{
		this.getThumb().style.top = parseInt(thumbPosition) + "px";
	}
	
	function _scrollDelta( deltaY )
	{
		if ( deltaY != 0 )
		{
			var newPosition = parseInt( this.thumbTop ) + parseInt( deltaY );
			
			if( parseInt(newPosition) < 0 )
			{
				newPosition = 0;
			}
			else if( parseInt( newPosition) > parseInt(this.scrollablePixels) )
			{
				newPosition = parseInt(this.scrollablePixels);
			}
			this.moveThumbTo( newPosition );

			var relativePos = parseFloat( newPosition / parseInt( this.scrollablePixels )  );
			
			// we message the view with a ratio of how much of our scrollable area we have scrolled to
			// it is up to the view to scroll itself in a way that makes sense for that ratio
			
			gPhoneBookMainView.scrollView( relativePos );
		}
	}
	
	function _mouseUp( event )
	{
		document.removeEventListener("mousemove", mouseMoveScrollbar, true);
		document.removeEventListener("mouseup", mouseUpScrollbar, true);
		this.isScrolling = false;
	}
	
	function _mouseDown( event )
	{
		document.addEventListener("mousemove", mouseMoveScrollbar, true);
		document.addEventListener("mouseup", mouseUpScrollbar, true);
		
		this.currentY = parseInt(event.y);

		this.updateThumbPosition();
		
		this.isScrolling = true;
	}
	
	function _mouseDrag( event )
	{
		var deltaY = parseInt(event.y) - parseInt(this.currentY);
		this.scrollDelta( deltaY );
	}
	
	function _channelClick( event )
	{
		// eventY is in absolute coordinates relative to the widget.
		// we need to correct this position by the scrollbar's absolute
		// vertical offset, before we can use the Y value
		var eventY = parseInt( event.y ) - parseInt(this.kChannelOffset);
		
		var viewRatio = gPhoneBookMainView.getViewToDocRatio();
		
		// we jump by fixed amounts for any given results view, but the delta depends on how much scrollable
		// range there is in the current results view (it varies between searches...)
		var deltaScroll = Math.round( parseInt(this.kScrollbarHeight) * viewRatio);
		
		// where is the thumb before we move?
		this.updateThumbPosition();
		
		// the only thing we are interested in was did eventY land above or below the thumb
		// we will jump the thumb up or down by the delta accordingly.
		if ( eventY < parseInt( this.thumbTop ) )
			deltaScroll = -deltaScroll;
			
		this.scrollDelta( deltaScroll );
		
		// where should the thumb be after we move?
		this.updateThumbPosition();
	}
	
	function _mouseWheelMoved( event )
	{
		// a mouse wheel moved event is similar to a channel click, but there is a range of wheel delta
		// which can be thought of as "acceleration."
		
		// You may wish to review the _channelClick() method for more info
		
		var scrollRatio = gPhoneBookMainView.getPixelToDocRatio(gConstMouseWheelScrollPixels); // pixel scroll per mouse wheel click
		
		// a channel click will jump the scroll delta by a fixed amount per click (but the value is dependent on the total height
		// of the current results view.
		// a mouse wheel move will jump the scroll delta a fixed pixel per click value, multiplied by the wheelDelta acceleration factor
		
		var wheelDelta = event.wheelDelta;
		
		if( wheelDelta < 0 )
			wheelDelta = Math.floor( wheelDelta/gConstMouseWheelDeltaDivisor );
		else if (wheelDelta > 0 )
			wheelDelta = Math.ceil( wheelDelta/gConstMouseWheelDeltaDivisor );

		var deltaScroll = ( Math.ceil( this.kScrollbarHeight * scrollRatio) * parseInt( wheelDelta ) );
		
		this.updateThumbPosition();
			
		this.scrollDelta( -deltaScroll );
		
		this.updateThumbPosition();
	}
		
	function _updateThumbPosition()
	{
		var thumb = this.getThumb();
		this.thumbTop = parseInt(document.defaultView.getComputedStyle( thumb,'' ).getPropertyValue('top'));
	}
	
	function _updateScrollbarForNewView( proportion )
	{

		var thumb = document.getElementById('scrollthumb');
		var channel = document.getElementById('scrollchannel');
		
		if (proportion == 0 || gResizeAnimation != null || gPhoneBookMainView.fFrontSideShowing == false )
		{
			if( channel )
				channel.style.visibility="hidden";
			if( thumb )
				thumb.style.visibility="hidden";
			
			document.removeEventListener("mousewheel", mouseWheelMoved, true);
		}
		else
		{
			var newHeight = Math.floor(parseInt(this.kScrollbarHeight) * proportion) - 7 /*topCap*/ - 6 /*bottomCap*/;
			
			if( channel )
				channel.style.visibility="visible";
			if( thumb )
				thumb.style.visibility="visible";
			
			document.getElementById('scroll-mid').style.height = (newHeight) + "px";
			
			this.updateThumbPosition();
			this.thumbHeight = parseInt(document.defaultView.getComputedStyle( thumb,'' ).getPropertyValue('height'));
			this.scrollablePixels = parseInt(this.kScrollbarBottom) - parseInt( this.thumbHeight ) - 1;
			
			document.addEventListener("mousewheel", mouseWheelMoved, true);
		}
		thumb.style.top=0; // reset
	}
	
	/*function _debugDump()
	{
		alert("debug dump CScroll instance...\r-----------------------");
		alert( "currentY : " + this.currentY );
		alert("thumbTop: " + this.thumbTop );
		alert("thumbHeight: " + this.thumbHeight );
		alert("scrollablePixels: " + this.scrollablePixels );
		alert("scrollChannelDelta: " + this.scrollChannelDelta );
		alert("kScrollbarBottom: " + this.kScrollbarBottom );
		alert( "kScrollbarHeight: " + this.kScrollbarHeight );
		alert("kChannelOffset: " + this.kChannelOffset );
	}*/
}

/*

A NOTE on widget expand and collapse animation:

	__________________________
   (__________________________)  // this is the collapsed state
   
   step 1 in expand is to animate a rect that grows downward in height to fill in the lower left and right corners.
   this is the "expanding box" animation
   
    __________________________
   /___________________________\ // this is the start of the expanded state
   \___________________________/ // the default round bg image is replaced by a top and bottom half
   
   step 2 in expand is to animate the overlapping of the bottom half over the top half.  The bottom half's top
   position starts out with a negative offset and it animates until the top position arrives at zero.
   The bottom half will have a top position of zero when it is not overlapping, and flush up against the top half.
   
    __________________________
   /___________________________\ // this is the final of the expanded state
   |___________________________|
   |___________________________| // the top and bottom halves are expanded by the presense of the mid body part
   |___________________________|
   \___________________________/
   
   step 3 in expand is to animate the height of the mid body part that grows downward and pushes the bottom half
   down with it.

   
   To collapse the widget, the steps are repeated in reverse.
   
   
   A sequence of animation is started by creating an animator object which animates either the start of the
   expand or collapse.  A "finalization" routine (onfinish) is part of the animator object.  Each step of the
   animation requires a new animator object with the appropriate data, so in a 3 step animation sequence, when
   each step completes, the "finalization" routine will set up the animation object for the next step and get it started.
   
   Think of this as a chain of animations, where each link chains up to the next animation step.
*/

function CAnimator( duration /*ms*/,
					posNow /*start pos*/, from, to, /*animate from some value to another*/
					element /*what is being animated?*/, animatedAttributeName, /*what style attribute of the element is animated?*/ 
					onfinish /*callback to finish animation*/)
{
	this.duration = duration;
	this.positionNow = posNow;
	this.positionFrom = from;
	this.positionTo = to;
	this.element = element;

	this.forcingEndStateFlag = false;	
	this.animatedAttributeName = animatedAttributeName; // "height" or "top" or other numerical style attribute?
	this.timer = null;

	this.animate = _animate;
	this.markStart = function() { this.startTime = (new Date).getTime() - 13; };
	
	this.startTime = this.markStart();
	
	this.onfinish = onfinish; // should be a function pointer
	
	this.runAnimation = _runAnimation;
	
	this.clear = _clear;
	
	this.forceEnd = _forceEnd;
	
	function _clear()
	{
		this.positionNow = 0;
		this.positionTo = 0;
		this.positionFrom = 0;
		this.element = null;
		this.animatedAttributeName = "";
		if( this.timer )
			clearInterval( this.timer );
		this.timer = null;
	}
	
	// try to hurry things up.  force the animation to finish.
	function _forceEnd()
	{
		this.forcingEndStateFlag = true;
		if( this.timer )
		{
			this.element.style[animatedAttributeName] = this.positionTo;
			this.positionNow = this.positionTo;
		
			this.onfinish();
		}
	}

	function _runAnimation()
	{
		if( this.element == undefined ) // don't animate non existent elements
		{
			clearInterval( this.timer );
			this.timer = null;
			alert("People: can't animate null element...");
			return;
		}
    	
		this.element.style.display = "block";
		this.element.style[this.animatedAttributeName] = this.positionNow;
		this.markStart();
		
		if( this.forcingEndStateFlag != true )
		{
			this.timer = setInterval ("gResizeAnimation.animate();", 13);
			this.animate();
		}
		else
		{
			this.onfinish();
		}
	}
	
	function _animate ()
	{
		try
		{
			if( this.element == undefined )
			{
				clearInterval( this.timer );
				this.timer = null;
				alert("People: can't animate null element...");
				return;
			}
			var T;
			var ease;
			var time  = (new Date).getTime();
			var newValue = null;
			var frame;
			var finished = false;
				
			T = limit_3(time-this.startTime, 0, this.duration);
			
			ease = 1.0 * (T / this.duration);
				
			// stop the animation if time exceeded the duration or the widget has been flipped..
			if((T >= this.duration) || this.positionNow == this.positionTo || ( !gPhoneBookMainView.fFrontSideShowing ))
			{	
				newValue = this.positionTo;
				
				clearInterval(this.timer);
				this.timer = null;
				finished = true;
			}
			else
			{
				newValue = computeNextFloat(this.positionFrom, this.positionTo, ease);
			}
			
			this.positionNow = parseInt(newValue);
			this.element.style[this.animatedAttributeName] = this.positionNow;
				
			if( finished )
			{
				if( this.timer )
					clearInterval(this.timer);
				this.timer = null;
				if( this.onfinish )
				{
					setTimeout( "gResizeAnimation.onfinish();", 0); // call after the last frame is drawn
				}
			}
		}
		catch(ex)
		{
			alert("People: Exception in animation timer...");
			if( this.timer )
				clearInterval(this.timer);
			this.timer = null;
		}
	}
}

/* Step 1 in expansion animation, on complete call this finish routine */
function finishPreRollExpandingBox()
{	
	var body = document.getElementById("bodyDiv");
	var topDiv = document.getElementById("topDiv");
	//var topBar = document.getElementById("topbar");
	var bottomBar = document.getElementById("bottombar");
	var bottomDiv = document.getElementById("bodyBottomDiv");
	
	var preRollBottomHalfPosNow = constPreRollBottomHalfMaxOverlapPos;
	var preRollBottomHalfStartPos = constPreRollBottomHalfMaxOverlapPos;
	var preRollBottomHalfEndPos = constPreRollBottomHalfMinOverlapPos;
	
	window.resizeTo(335, gConstWidgetExpandedHeight); // open up the clipping region
	
	topDiv.style.height = "65px"; // change from collapsed to expanded top part
	//topBar.src = "Images/bodycompact_expanded.png";
	bottombar.src = "Images/body_midsolid.png";
	bottombar.style.top = "40px";
	bottombar.style.height = "25px";
	
	// hide the expanding box thing
	gResizeAnimation.element.style.display = "none";
	
	// open up the full body area
	body.style.height = gConstWidgetExpandedHeight;
	body.style.visibility="visible";
	
	// show the bottom part
	bottomDiv.style.top= constPreRollBottomHalfMaxOverlapPos + "px";
	bottomDiv.style.display="block";
	
	var forcingEndOfAnimation = gResizeAnimation.forcingEndStateFlag; // pass this on to the next animation in the chain
	
	gResizeAnimation.clear();
	gResizeAnimation = null;
	
	// set up for Step 2 in expansion animation
	gResizeAnimation = new CAnimator( constPreRollOverlapAnimationDuration, preRollBottomHalfPosNow, preRollBottomHalfStartPos, preRollBottomHalfEndPos, bottomDiv, 'top', finishPreRollExpandOverlapTopAndBottom );

	gResizeAnimation.sequenceName = "PreRollOverlapAnimation";
	gResizeAnimation.forcingEndStateFlag = forcingEndOfAnimation;
	
	gResizeAnimation.runAnimation(); // start Step 2
}

/* Step 2 in expansion animation, on complete call this finish routine */
function finishPreRollExpandOverlapTopAndBottom()
{	
	var bottomDiv = document.getElementById("bodyBottomDiv");
	
	var bottomBar = document.getElementById("bottombar");
	bottombar.src = "Images/body_citystate_spacer.png";
	bottombar.style.top = "40px";
	bottombar.style.height = "25px";
	
	bottomDiv.style.position="relative";
	bottomDiv.style.top=constPreRollBottomHalfMinOverlapPos+"px";
	
	
	var forcingEndOfAnimation = gResizeAnimation.forcingEndStateFlag; // pass this on to the next animation in the chain
	
	gResizeAnimation.clear();
	gResizeAnimation = null;
	
	var bodyMid = document.getElementById("bodyMidDiv");
	bodyMid.style.height = gConstWidgetCollapsedMidHeight;	

	document.getElementById('extraTopLedgerLineImg').style.display="inline-block";
	document.getElementById('extraBottomLedgerLineImg').style.display="inline-block";
	
	// set up for Step 3 in expansion animation
	gResizeAnimation = new CAnimator( constMainResizeAnimationDuration, gConstWidgetCollapsedMidHeight, gConstWidgetCollapsedMidHeight,
											 gConstWidgetExpandedMidHeight, bodyMid, 'height', mainAnimationFinished);
	gResizeAnimation.forcingEndStateFlag = forcingEndOfAnimation;
	gResizeAnimation.runAnimation(); // start Step 3
}

/* Step 2 in collapse animation, on complete call this finish routine */
function finishPreRollCollapseOverlapTopAndBottom()
{
	var preRollExpandingBoxHeightNow = constPreRollExpandingBoxFullHeight;
	var preRollExpandingBoxStartHeight = constPreRollExpandingBoxFullHeight;
	var preRollExpandingBoxEndHeight = constPreRollExpandingBoxMinHeight;
	
	var expandingBox = document.getElementById("expandAnimationBlock");
	
	gResizeAnimation.element.style.display = "none";
	
	var forcingEndOfAnimation = gResizeAnimation.forcingEndStateFlag; // pass this on to the next animation in the chain
	
	gResizeAnimation.clear();
	gResizeAnimation = null;
	
	// set up for Step 3 in collpse animation
	gResizeAnimation = new CAnimator( constPreRollExpandingBoxAnimationDuration /*duration*/, preRollExpandingBoxHeightNow, preRollExpandingBoxStartHeight/*from*/, preRollExpandingBoxEndHeight/*to*/,
											 expandingBox /*elem*/, 'height', finishPreRollCollapseExpandingBox );
	gResizeAnimation.forcingEndStateFlag = forcingEndOfAnimation;

	// before running step 3, we change the top image to the compact version
	document.getElementById("topbar").src = "Images/bodycompact_expanded.png";
	
	gResizeAnimation.runAnimation(); // start Step 3
}

/* Step 3 in collapse animation, on complete call this finish routine */
function finishPreRollCollapseExpandingBox()
{

	gResizeAnimation.element.style.display = "none";
	gResizeAnimation.clear();

	document.getElementById("bodyBottomDiv").style.display = "none";
	var bottombar = document.getElementById("bottombar");
	
	bottombar.src = "Images/body_bottom_2.png";
	bottombar.style.top = "40px";
	bottombar.style.height = "36px";
		
	// set height to the collapsed height since 
	if( gPhoneBookMainView.fFrontSideShowing )
	{
		document.getElementById("topDiv").style.height = gConstWidgetCompactHeight;
		window.resizeTo (335, gConstWidgetCompactHeight);
	}
	
	gResizeAnimation = null; // must null gResizeAnimation at finish of animation...
}

/* Step 1 in collapse animation, AND step 3 in expand animation - on complete call this finish routine */

function mainAnimationFinished()
{ 
	var forcingEndOfAnimation = false;
	if( gResizeAnimation )
	{
		forcingEndOfAnimation = gResizeAnimation.forcingEndStateFlag;

		if( forcingEndOfAnimation )
			gResizeAnimation.element.style[gResizeAnimation.animatedAttributeName] = gResizeAnimation.positionTo;
		
			
		gResizeAnimation.clear();
		gResizeAnimation = null;
	}

	if (window.widget)
	{
		if( gPhoneBookMainView.fIsCollapsed) // if we are collapsing, we chain to the next animated step
		{
			var preRollBottomHalfPosNow = constPreRollBottomHalfMinOverlapPos;
			var preRollBottomHalfStartPos = constPreRollBottomHalfMinOverlapPos;
			var preRollBottomHalfEndPos = constPreRollBottomHalfMaxOverlapPos;
			
			var bottomDiv = document.getElementById("bodyBottomDiv");
			
			gResizeAnimation = new CAnimator( constPreRollOverlapAnimationDuration, preRollBottomHalfPosNow, preRollBottomHalfStartPos, preRollBottomHalfEndPos,
													 bottomDiv, 'top', finishPreRollCollapseOverlapTopAndBottom );
			
			gResizeAnimation.forcingEndStateFlag = forcingEndOfAnimation;
			
			document.getElementById('extraTopLedgerLineImg').style.display="none";
			document.getElementById('extraBottomLedgerLineImg').style.display="none";
			
			bottomDiv.style.display="block";
			bottomDiv.style.top=preRollBottomHalfPosNow;
			
			gResizeAnimation.forcingEndStateFlag = forcingEndOfAnimation;
			
			gResizeAnimation.runAnimation();
			
		}
		else // widget expanded, and this was the last step
		{
			document.getElementById('extraTopLedgerLineImg').style.display="block";
			document.getElementById('extraBottomLedgerLineImg').style.display="block";
			
			//focusInputField();
			
			gPhoneBookMainView.updateViewGeometryInfo();
			updateScrollbar();
		}
	}
}


// when we wait for a location validation query to reply, we periodically call this function to animate
// a status message.  The timer is stopped in the handler for the reply (or it eventually quits automatically?)
var gStatusAnimationTimerData = null;
function statusAnimationTimer ()
{
	
	if( gStatusAnimationTimerData == null )
		return;
		
	var statusField = document.getElementById(gStatusAnimationTimerData.elementID); // show the message
	statusField.style.visibility="visible";

	// animate the ellipses...
	
	// dotCount goes 0 <-> 3
	gStatusAnimationTimerData.dotCount += gStatusAnimationTimerData.forward ? 1 : -1;
	
	if (gStatusAnimationTimerData.dotCount > 3)
	{
		gStatusAnimationTimerData.forward = false;
		gStatusAnimationTimerData.dotCount = 3;
	}
	else if (gStatusAnimationTimerData.dotCount < 0)
	{
		gStatusAnimationTimerData.forward = true;
		gStatusAnimationTimerData.dotCount = 1;
	}
	
	var text = getLocalizedString(gStatusAnimationTimerData.localizedStringKey);
	for (var i = 0; i < gStatusAnimationTimerData.dotCount; ++i)
		text+= '.';
	
	statusField.innerText = text;
}

function clearStatusAnimationTimer()
{
	if( gStatusAnimationTimerData != null )
	{	
		if( gStatusAnimationTimerData.elementID == 'statusTextDiv' )
		{
			// by default, statusTextDiv should be center aligned, unless our "Searching..." timer is running
			document.getElementById('statusTextDiv').style["text-align"]="center";
			document.getElementById('statusTextDiv').style["left"] = "18px";
		}
		else if( gStatusAnimationTimerData.elementID == 'validate')
		{
			// we hide validate element, but other status lines may not hide after completion
			document.getElementById('validate').style.visibility="hidden";
		}
		if( gStatusAnimationTimerData.timer )
			clearInterval(gStatusAnimationTimerData.timer);
		gStatusAnimationTimerData = null;
	}
}

function getLocalizedString(key) {
	if ( typeof localizedStrings[key] != 'undefined') {
		return localizedStrings[key];
	}
	return key;
}

// get the user's address from the "me" card in Addressbook
function getUserAddress()
{
	if (document.embeds['citystate-input'].getSubPlugin != null && gCustomSearchAddressSet == false )
	{	
		var addressInfoPreEncode = null;
		
		var constCityIndex = 0
		var constStateIndex = 1;
		var constZIPIndex = 2;
		
		// get the address set in "my card" of the current user's AddressBook
		var addressInfoPreEncode = document.embeds['citystate-input'].getSubPlugin().WPgetUserAddress();

		// make sure it is really empty...a reset
		while(gDefaultSearchFromAddress.length)
		{
			gDefaultSearchFromAddress.pop();
		}

		// actually, our plugin never returns null even when the "my" card in AB is deleted
		// but we will check and set a default here, in case the plugin behavior ever changes
		if( addressInfoPreEncode == null )
			addressInfoPreEncode = ",,"; // "city,state,zip"

			
		// we want either a ZIP, or a city & state
		
		// we prefer ZIP.  If we have zip, we're going to leave everything else blank
		// This is important, because the backside input prefers zip also, and maintaining
		// city, state, and zip at the same time for our user address puts us out of synch 
		// with that method, and it somethings thinks our address changed when it didn't
		// because it deletes city and state from the input if it finds a zip.
		
		if( addressInfoPreEncode[constZIPIndex].length > 0 ) // we have ZIP - we don't need anything else!
		{
			/*gCurrentSearchFromStreet = "";
			gCurrentSearchFromCity = "";
			gCurrentSearchFromState = (addressInfoPreEncode[constStateIndex] + '').toUpperCase();
			gCurrentSearchFromZip = addressInfoPreEncode[constZIPIndex] + '';*/
			
			// build an address that has zip only
			gDefaultSearchFromAddress.push(""); // street
			gDefaultSearchFromAddress.push(""); // city
			gDefaultSearchFromAddress.push(URLEncode((addressInfoPreEncode[constStateIndex] + '').toUpperCase())); // state
			gDefaultSearchFromAddress.push(URLEncode(addressInfoPreEncode[constZIPIndex] + '')); // zip
		
		}
		else if ( addressInfoPreEncode[constCityIndex].length <= 0 ) // no ZIP AND no city means we don't have enough info, so just use a default ZIP
		{
			/*gCurrentSearchFromStreet = "";
			gCurrentSearchFromCity = "";
			gCurrentSearchFromState = "";
			gCurrentSearchFromZip = constZipIfNoUserLocation;*/
			
			// build an address that has zip only
			gDefaultSearchFromAddress.push(""); // street
			gDefaultSearchFromAddress.push(""); // city
			gDefaultSearchFromAddress.push(""); // state
			gDefaultSearchFromAddress.push(""); // zip
		}
		else // no ZIP, but city was ok, so we'll go with that and provide a default state if missing
		{
			/*gCurrentSearchFromStreet = "";
			
			gCurrentSearchFromCity = convertToInitialCaps( addressInfoPreEncode[constCityIndex] );
			
			if( addressInfoPreEncode[constStateIndex].length > 0 )
				gCurrentSearchFromState = (addressInfoPreEncode[constStateIndex] + '').toUpperCase();
			else
				gCurrentSearchFromState = constStateIfNoUserState;
				
			gCurrentSearchFromZip = "";*/

			gDefaultSearchFromAddress.push(""); // street
			gDefaultSearchFromAddress.push(URLEncode(convertToInitialCaps( addressInfoPreEncode[constCityIndex] ))); // city
			
			if( addressInfoPreEncode[constStateIndex].length > 0 )
				gDefaultSearchFromAddress.push(URLEncode((addressInfoPreEncode[constStateIndex] + '').toUpperCase()));
			else
				gDefaultSearchFromAddress.push("");
			
			gDefaultSearchFromAddress.push(""); // zip
		}
	}
	return gDefaultSearchFromAddress; // if anything went wrong, this should have a default defintion already
}

function addToAddressBook( address )
{
	if (document.embeds['citystate-input'].getSubPlugin())
		document.embeds['citystate-input'].getSubPlugin().WPaddToAddressBook(address.firstname, address.lastname, address.address, address.city, address.state, address.zip, address.phone);
}

function cancelModals()
{
	popupDialogCancelPressed(null);
}

function popupDialogConfirmPressed(event)
{
	if( gLineViews != null && gLineViews.addToAddressBookTarget != null )
	{
		// we have a reference to a target html element displayed in the widget
		// which has attributes containing the address info we need.
		var addAddressInfo = { firstname: gLineViews.addToAddressBookTarget.getAttribute("AB_firstname"),
							   lastname: gLineViews.addToAddressBookTarget.getAttribute("AB_lastname"),
							   address: gLineViews.addToAddressBookTarget.getAttribute("AB_address"),
							   city: gLineViews.addToAddressBookTarget.getAttribute("AB_city"),
							   state: gLineViews.addToAddressBookTarget.getAttribute("AB_state"),
							   zip: gLineViews.addToAddressBookTarget.getAttribute("AB_zip"),
							   phone: gLineViews.addToAddressBookTarget.getAttribute("AB_phone") };
							   
		addToAddressBook( addAddressInfo );
		
		// disable further clicks and rollover once the image is clicked - this will only work while current results displayed
		gLineViews.addToAddressBookTarget.onclick = function(event) { /*pbAlert("already added this item to addressbook");*/ };
		gLineViews.addToAddressBookTarget.onmouseover = function(event) { };
		gLineViews.addToAddressBookTarget.onmouseout = function(event) { };
		
		// set the style of the + image to indicate that it is no longer active
		
		gLineViews.addToAddressBookTarget.style.visibility = "hidden";
		
		// delete the reference to the target of the confirm dialog
		gLineViews.addToAddressBookTarget=null;
	}
	// hide the confirm dialog
	document.getElementById("popupDialog").style.visibility = "hidden";

}

function popupDialogCancelPressed(event)
{
	if( gLineViews != null && gLineViews.addToAddressBookTarget != null )
	{
		gLineViews.addToAddressBookTarget.style.background="url(Images/yp_add_onMedium.png) no-repeat center center";
		
		// delete the reference to the global target for the confirm dialog, and the global address to save info
		gLineViews.addToAddressBookTarget=null;
		
		// hide the confirm dialog
		document.getElementById("popupDialog").style.visibility = "hidden";
	}
}

function focusInputField()
{
	document.embeds['citystate-input'].setFocus();
	focusCityState();
}

function getFirstName()
{
	return URLEncode(trim(document.embeds['firstName-input'].value()));
}

function getLastName()
{
	return URLEncode(trim(document.embeds['lastName-input'].value()));
}

var kDataPerSearch = 24;

// note that the range limit unit is "miles", which is what daplus.us uses.  In other locales with other service providers, a change in units might be required.
var gLastSearchData = { feedType:"daplus.us", queryTask:"search", firstName:null, lastName:null, categoryMenuIndex:0, categorySIC:null,
						rangeLimit:0, RecordsFrom:0, RecordsTo:(kDataPerSearch-1), totalPossibleRecords:-1 };
						
var gLastCityState = "";

function getNewSearchData( newSearch )
{	
	var from = 0;
	var to = kDataPerSearch;
	
	//var searchTerm = getInputFieldValue();
	var fn = getFirstName(); //already trimmed
	var ln = getLastName();  //already trimmed
	
	var rangeLimit = gPhoneBookMainView.getCurrentRangeLimit();
	var categoryMenuIndex = 0;
	
	// is it a new search, or a continuation of a previous one?
	if( newSearch )
	{
		gLastSearchData.RecordsFrom = from;
		gLastSearchData.RecordsTo = to;
		gLastSearchData.categoryMenuIndex = categoryMenuIndex;
		gLastSearchData.categorySIC = null;
		gLastSearchData.totalPossibleRecords=-1;
	
		// make it acceptable for passing in a url
		//searchTerm = URLEncode( searchTerm );//!!!?
		gLastSearchData.firstName = fn;
		gLastSearchData.lastName = ln;
		gLastSearchData.rangeLimit = gPhoneBookMainView.getCurrentRangeLimit();
	}
	else
	{
		gLastSearchData.RecordsFrom = parseInt(gLastSearchData.RecordsFrom) + kDataPerSearch;
		
		// if the feed is returning total possible records 0, that might not be actually true...
		if( (gLastSearchData.totalPossibleRecords == -1) || gLastSearchData.totalPossibleRecords == 0 || (parseInt(gLastSearchData.RecordsTo) + kDataPerSearch < gLastSearchData.totalPossibleRecords) )
			gLastSearchData.RecordsTo = parseInt(gLastSearchData.RecordsTo) + kDataPerSearch;
		else
		{
			gLastSearchData.RecordsTo=(gLastSearchData.totalPossibleRecords);
			if( gLastSearchData.RecordsFrom > gLastSearchData.RecordsTo )
				gLastSearchData.RecordsFrom = gLastSearchData.RecordsTo;
		}
		
	}
	var searchData = { feedType:gLastSearchData.feedType, queryTask:gLastSearchData.queryTask, firstName:fn, lastName:ln, 
						categoryMenuIndex:gLastSearchData.categoryMenuIndex, categorySIC:gLastSearchData.categorySIC, rangeLimit:gLastSearchData.rangeLimit,
						RecordsFrom:gLastSearchData.RecordsFrom, RecordsTo:gLastSearchData.RecordsTo };
	return searchData;
}


var gValidateCitySearchData = { feedType:"daplus.us", queryTask:"locationValidation", searchTerm:"qxqzxqxqxzq" /*intentionally non matching*/,
								categoryMenuIndex:0, categorySIC:0, rangeLimit:0, RecordsFrom:0, RecordsTo:100, totalPossibleRecords:-1 };

function cityStateChanged()
{		
	if (gSettingFlag)
	{
		gSettingFlag = false;
		return;
	}
	
	var cityState = document.embeds['citystate-input'];	
	var cityStateZip = cityState.value();
	
	if (cityStateZip.length > 0) 
	{
		if (cityStateZip == gLastCityState)
		{
			gTabbingFlag = false;
			searchInputChanged();
			return;
		}
		
		var addressRecord = {city:"", state:"", zip:""};
		addressRecord = parseAddress(cityStateZip); // pull out the city, state, and the zip
		
		if( addressRecord.zip.length <=0 ) // we will only handle city and state if there is no zip
		{
			if (addressRecord.state.length > 0 || addressRecord.city.length > 0)
			{
				var addressArray = new Array;
				
				addressArray.push(""); // no street address
				addressArray.push(URLEncode( addressRecord.city ));
				
				// if user did not supply a state, we'll use the last known state
				if( addressRecord.state.length > 0 )
					addressArray.push(URLEncode(addressRecord.state.toUpperCase()));
				else if (gDefaultSearchFromAddress && gDefaultSearchFromAddress[2] && gDefaultSearchFromAddress[2].length > 0)
					addressArray.push(URLEncode(gDefaultSearchFromAddress[2].toUpperCase()));
				else
					addressArray.push("");
									
				addressArray.push(""); // zip is blank
				
				// We'll try to validate the city and state by querying to our feed provider					
				var query = buildSearchRequest( gValidateCitySearchData, addressArray );
				performXMLRequest( "locationValidation", query, locationValidationCallback );
			}
			else // no state?  just set our data for failure and call the validation callback directly
			{
				gLocationValidationData = { error:"insufficient", locationMatchData:null, totalPossibleResults:0 };
				locationValidationCallback();
			}
		}
		else
		{
			searchInputChanged();
			gTabbingFlag = false;
		}
	}
	else
	{
		gTabbingFlag = false;
		cityState.setCancelImage(2);
	}
}

var gLocationValidationData = null;
function locationValidationCallback(locationData, errMsg)
{
	// we're going  to either input a single result into the input field, or
	// we''ll display a menu with some city, state options...
    
    if (!gPhoneBookMainView.fFrontSideShowing)
    	return;
    
    gLocationValidationData = locationData;
    
	if( gLocationValidationData != null && gLocationValidationData.error != "validated" )
	{	
		var menu = null;
		
		if (window.widget)
			menu = widget.createMenu();
			
		var cityStateZip = document.embeds['citystate-input'].value();
		
		var addressRecord = {city:"", state:"", zip:""};
		
		// input should be non zero, and it should be different from what we have already
		if( cityStateZip.length > 0 )
			addressRecord = parseAddress(cityStateZip); // pull out the city, state, and the zip
		
		// errors handled here
		if( gLocationValidationData == null || gLocationValidationData.error == "insufficient" || gLocationValidationData.error || gLocationValidationData.locationMatchData == null
			|| gLocationValidationData.locationMatchData.length <= 0 )
		{
			if ( menu != null )
			{	
				menu.addMenuItem( getLocalizedString("No cities found" ) );
				menu.setMenuItemEnabledAtIndex( 0, false );
			}
		}
		else // do we have some results
		{
			var warningWasAddedToMenu = false;
			
			var c = gLocationValidationData.locationMatchData.length;
			
			if (c == 1 || window.widget === undefined) // one result?  set it in the input field
			{
				// just set the contents if their is only one city.
				var city = gLocationValidationData.locationMatchData[0].city;
				var state = gLocationValidationData.locationMatchData[0].state;
				
				if(  menu != null && addressRecord.state <= 0 ) // user did not specify state, so prompt them to do the right thing
				{
					warningWasAddedToMenu = true;
					menu.addMenuItem( getLocalizedString('Specify City, State or ZIP Code:'));
					menu.setMenuItemEnabledAtIndex (0, false);
					//if (gDefaultSearchFromAddress[2].length > 0)
						menu.addMenuItem( convertToInitialCaps( city ) + ", " + gDefaultSearchFromAddress[2]);
					//else
					//	menu.addMenuItem( convertToInitialCaps( city ) + ", " + constStateIfNoUserState);
				}
				else
				{
					menu = null;
					gSettingFlag = true;
					document.embeds['citystate-input'].setValue(city + ", " + state);
					document.embeds['citystate-input'].setCancelImage(1);
					gSettingFlag = false;
				}
			}
			else // multiple results?  load them into a menu
			{
				if(  menu != null  && addressRecord.state.length <= 0 )
				{
					warningWasAddedToMenu = true;
					menu.addMenuItem( getLocalizedString('Specify City, State or ZIP Code:'));
					menu.setMenuItemEnabledAtIndex (0, false);
				}
				
				for (var i = 0; i < c; ++i)
				{
					var city = gLocationValidationData.locationMatchData[i].city;
					var state = gLocationValidationData.locationMatchData[i].state;
					menu.addMenuItem (city + ", " + state);
				}
			}
			
		}
			
		if (menu != null) // if the menu was loaded, we're going to pop it up
		{
			/*if( addressRecord.state.length <= 0 && addressRecord.city.length > 0 )
			{
				gSettingFlag = true;
				document.embeds['citystate-input'].setValue(convertToInitialCaps( addressRecord.city ) + ", " + gCurrentSearchFromState);
				gSettingFlag = false;
			}*/
			
			var selectedItem = menu.popup(60, 63); // this function does not return til the user makes a selection
			
			if (selectedItem == -1)
			{
				gLocationValidationData = null;
				gTabbingFlag = false;
				gValidationFailed = true;
				document.embeds['citystate-input'].setCancelImage(2);
				return;
			}
		
			if( warningWasAddedToMenu ) // we inserted a message at index zero in the menu
				selectedItem--;
				
			if ( selectedItem >= 0 ) // what did they pick?  set that result to the input field if legitimate...
			{
				var city = gLocationValidationData.locationMatchData[ selectedItem ].city;
				var state = gLocationValidationData.locationMatchData[ selectedItem ].state;
				gSettingFlag = true;
				document.embeds['citystate-input'].setValue(city + ", " + state);
				document.embeds['citystate-input'].setCancelImage(1);
				gSettingFlag = false;
			}
		}
		gLocationValidationData = null;
		if (!gTabbingFlag)
			searchInputChanged();
		gTabbingFlag = false;
	}
	else
	{
		var cityStateZip = document.embeds['citystate-input'].value();
		var addressRecord = {city:"", state:"", zip:""};
		if( cityStateZip.length > 0 )
			addressRecord = parseAddress(cityStateZip);
		if (addressRecord.state.length == 0)
			document.embeds['citystate-input'].setCancelImage(2);
		else
		{
			// we have a state
			if (!gTabbingFlag)
				searchInputChanged();
		}
		gLocationValidationData = null;
		gTabbingFlag = false;
	}
}

function searchInputChanged()
{
	cancelModals();
	var cityState = document.embeds['citystate-input'].value();
	var fn = getFirstName();
	var ln = getLastName();
	
	if (cityState.length == 0)
	{
		//searchInputCancelled();
		document.embeds['citystate-input'].setCancelImage(2);
		if (ln.length == 0)
			document.embeds['lastName-input'].setCancelImage(2);
		return;
	}
	
	if ((fn != gLastSearchData.firstName || ln != gLastSearchData.lastName || cityState != gLastCityState) && (ln.length > 0 && cityState.length > 0))
	{
		//gLastSearchData.searchTerm = getInputFieldValue(); // set this early to help prevent race condition
		gLastSearchData.firstName = fn;
		gLastSearchData.lastName = ln;
		gLastCityState = cityState;
		newSearch();
		return;
	}
	
	if (ln.length == 0)
		document.embeds['lastName-input'].setCancelImage(2);
}

function searchInputCancelled()
{
	cancelModals();
	
    gLastSearchData.firstName = "";
    gLastSearchData.lastName = "";
    document.embeds['citystate-input'].setValue("");
	gPhoneBookMainView.reset();
	//if( gResizeAnimation == null )
	gPhoneBookMainView.growView(false, false);
	document.embeds['citystate-input'].setCancelImage(0);
}

function cityStateInput()
{
	//if (document.embeds['citystate-input'].value().length == 0)
	//	searchInputCancelled();
	var val = document.embeds['citystate-input'].value();
	var state;
	if (val.length > 0)
		state = 1;
	else
		state = 0;
	document.embeds['citystate-input'].setCancelImage(state);
}

function newSearch() 
{
	// don't do searches when flipped to back

	if( gPhoneBookMainView.fFrontSideShowing )
	{
		//var newSearchTerm = getInputFieldValue();
		
		//cityStateValidate();
		
		if( getLastName().length > 0 && document.embeds['citystate-input'].value().length > 0) // don't search with blank strings, collapse instead
		{
			gPhoneBookMainView.reset();
			
			if( gResizeAnimation == null )	// don't grow if already animating, as it can lead to race conditions
				gPhoneBookMainView.growView(true, false);
			
			var searchData = getNewSearchData( true );
			performSearch( searchData, true );
		}
		else // blank search term?  collapse...
		{
			gPhoneBookMainView.reset();
			if( gResizeAnimation == null )
				gPhoneBookMainView.growView(false, false);
		    //gLastSearchData.searchTerm = "";
		    gLastSearchData.firstName = "";
    		gLastSearchData.lastName = "";
    	}
    	
    	//focusInputField(); // don't refocus the field if we're on the flipside
    }
}

function performSearch( searchData, newSearch )
{	
	
	var addressInfo = null;
	
	//searchTerm = searchData.searchTerm;
	var ln = searchData.lastName;
	var fn = searchData.firstName;
	
	gPhoneBookMainView.updateArrows();
	updateStatusText( true, 0, 0 );
	
	if ( ln && ( gLastSearchData.feedType == "daplus.us" || ln.length > 0 ) )
	{
		var userAddress;
		//var cityStateZip = document.getElementById('citystate-input').value;
		var cityStateZip = document.embeds['citystate-input'].value();
		
		if ( cityStateZip.length > 0 )
		{
			var addressRecord = {city:"", state:"", zip:""};
			addressRecord = parseAddress(cityStateZip); // pull out the city, state, and the zip
			if( addressRecord.zip.length <=0 ) // we will only handle city and state if there is no zip
			{
				if( addressRecord.state.length > 0 || addressRecord.city.length > 0)
				{
					var addressArray = new Array;
					addressArray.push(""); // no street address
					addressArray.push(URLEncode( addressRecord.city ));
					
					if( addressRecord.state.length > 0 )
						addressArray.push(URLEncode(addressRecord.state.toUpperCase()));
					else if (gDefaultSearchFromAddress && gDefaultSearchFromAddress[2] && gDefaultSearchFromAddress[2].length > 0)
						addressArray.push(URLEncode(gDefaultSearchFromAddress[2].toUpperCase()));
					else
						addressArray.push("");
					
					addressArray.push(""); // zip is blank
					userAddress = addressArray;
				}
			}
			else
			{
				userAddress = new Array;
				userAddress.push(""); // street blank;
				userAddress.push(""); // city blank;
				userAddress.push(""); // state blank;
				userAddress.push(addressRecord.zip); 
			}
		}
		else
			userAddress = getUserAddress();

		//if ( window.widget && searchTerm ) 
		//	widget.setPreferenceForKey(searchTerm,"lastSearchTerm");
		if( newSearch == true )
		{
			gPhoneBookMainView.reset();
		}
		
		// need to save here so it can be used by the parser
		gLastQuery = buildSearchRequest(searchData, userAddress);
		fetchPhoneBookData();
				
		clearStatusAnimationTimer(); // this will end and delete any other status animation timers that may be running
		
		// set the message
		var searching = document.getElementById('statusTextDiv');
		searching.innerText = getLocalizedString('Searching');
		searching.style["text-align"]="left";
		searching.style["left"] = "41px";
		
		// set the animation data and fire off the timer, which will callback to validateTimer() periodically
		gStatusAnimationTimerData = {timer:setInterval('statusAnimationTimer();', 500), dotCount:3, forward:true, elementID:'statusTextDiv', localizedStringKey:"Searching"};
		
	}
	gLastSearchData.firstName = fn;
	gLastSearchData.lastName = ln;
}

function dataLoaded( data, errorString )
{	
	// the timer may have been deleted or recycled for use by another animation.  Only terminate the timer if it is ours
	if (gStatusAnimationTimerData != null && gStatusAnimationTimerData.elementID == 'statusTextDiv')
	{
		document.getElementById('statusTextDiv').innerText = ""; // we set it to blank, and later it will contain the results range
		clearStatusAnimationTimer();
	}
	
    if( data != null )
    {
    	// data.business.length may be zero on a second iteration through the search loop, because there are no MORE results.
    	// If it is zero on the first iteration (identified because we have zero results found so far)
    	// then that means there are no results...
    	if( data.businesses.length <= 0 && gCacheData.getLength() <= 0 )	
		{
			gPhoneBookMainView.setNoResults(getLocalizedString('No Results'));
		}
        else
        {
        	gLastSearchData.totalPossibleRecords=data.totalPossibleResults;

			var rangeLimit = gPhoneBookMainView.getCurrentRangeLimit();

			var dataCountAdded = 0;

			// adding data to cache...
			dataCountAdded = gCacheData.addData( data );

			// how many result items did we get so far, including if we looped back in this function again during one search
			gPhoneBookMainView.fDataResultsCountOfOneSearchLoop = parseInt( gPhoneBookMainView.fDataResultsCountOfOneSearchLoop ) + parseInt( dataCountAdded );

			var partialDataSetReturned = data.businesses.length < (kDataPerSearch-1);
			
			var rangeLimitExceeded = (parseFloat( gCacheData.fLargestDistance ) != 0) && parseFloat( gCacheData.fLargestDistance ) > parseFloat( rangeLimit );
	
			var cacheFullEnough = ( parseInt( gPhoneBookMainView.fDataResultsCountOfOneSearchLoop ) >= ( parseInt( gPhoneBookMainView.fDataDisplayResultsPerPage ) + 10) );		
		
			// do we need more search results to fulfill the user's preferred results count per page?
			if( /*foundAllResults != true &&*/ partialDataSetReturned != true && rangeLimitExceeded != true && cacheFullEnough != true )
			{
				// if we need more results, we call for another search, which will eventually
				// recurse back into this function (so beware of infinite loops!)
				var searchData = getNewSearchData(false);
				performSearch( searchData, false);
			}
			else
			{
				// every time we finish searching and loading data, we will advance to the next display index
				// update indices AFTER adding data, because the cache will cap the upper index
				// at the current data length

				gPhoneBookMainView.setDisplayIndicesToNext();

				// if we end up searching again on the same key (when cache runs out)
				// we need to have this reset to zero, or the search will end prematurely
				gPhoneBookMainView.fDataResultsCountOfOneSearchLoop = 0;

				// data should be loaded
				
				try
				{
					gPhoneBookMainView.build();
				}
				catch(ex)
				{
					alert("People: exception while building the view...");
				}

				if( gLineViews.length <= 0 )
				{
					gPhoneBookMainView.setNoResults(getLocalizedString('No Results'));
				}
			}
		}
    }
    else if( gCacheData.getLength() > 0 )
    {
    	// Perhaps the server is returning us null data after we already completed some
    	// successful searches for the current search term.
    	
    	// If we have cached data already, lets' display it, rather than posting a 'no results found' message...
    	
		gPhoneBookMainView.fDataResultsCountOfOneSearchLoop = 0;

		// data should be loaded
		gPhoneBookMainView.build();

		if( gLineViews.length <= 0 )
		{
			gPhoneBookMainView.setNoResults(getLocalizedString('No Results'));
		}
    }
    else
    {
    	// our search came up null, and we have no cached data.  No results found...
    	
		gPhoneBookMainView.setNoResults("Data unavailable.");
    }
}

/*function isState(s)
{
	var i = 0, j = gStates.length;
	s = s.toUpperCase();
	for (i = 0; i < j; i++)
	{
		if (gStates[i] == s)
			return true;
	}
	return false;
}*/

function parseAddress( customAddress )
{
	var addressParts = customAddress.split(",");
	var addressRecord = { address:"", city:"", state:"", zip:"" };
	var numbersOnlyRegExp = /[0-9]{5}/i;
	
	if( addressParts != null && addressParts.length > 1 )
	{
		var zipCheck = null;
		
		if(addressParts[0].length > 0 )
		{
			zipCheck = addressParts[0].match( numbersOnlyRegExp );
			
			if( zipCheck != null )
			{
				addressRecord.zip = zipCheck.toString();
				addressParts[0] = addressParts[0].replace(numbersOnlyRegExp,"");
				addressParts[0] = trim( addressParts[0] );
				if( addressParts[0].length > 0 )
					addressRecord.city = addressParts[0];
			}
			else
			{
				addressRecord.city = trim( addressParts[0] );
			}
		}
		
		if( addressParts[1].length > 0 && zipCheck == null  )
		{
			zipCheck = addressParts[1].match( numbersOnlyRegExp );
			if( zipCheck != null )
			{
				addressRecord.zip = zipCheck.toString();
				addressParts[1] = addressParts[1].replace(numbersOnlyRegExp,"");
				addressParts[1] = trim( addressParts[1] );
				if( addressParts[1].length > 0 && !addressRecord.city.length > 0 )
					addressRecord.city = addressParts[1];
				else if( addressParts[1].length > 0 )
					addressRecord.state = addressParts[1];
			}
			else if( !addressRecord.city.length > 0 )
			{
				addressRecord.city = trim( addressParts[1] );
			}
			else if( !addressRecord.state.length > 0 )
			{
				var state = trim( addressParts[1] );
				var lookup = gStates[state.toUpperCase()];
				if (lookup != null)
					addressRecord.state = lookup;
				else
					addressRecord.state = state;
			}
		}
		else
			addressRecord.state = trim( addressParts[1] );

		if( addressParts.length > 2 && addressParts[2].length > 0 && zipCheck == null )
		{
			zipCheck = addressParts[2].match( numbersOnlyRegExp );
			if( zipCheck != null )
			{
				addressRecord.zip = zipCheck.toString();
				addressParts[2] = addressParts[2].replace(numbersOnlyRegExp,"");
				addressParts[2] = trim( addressParts[2] );
				if( addressParts[2].length > 0 && !addressRecord.state.length > 0)
					addressRecord.state = addressParts[2];
			}
			else if( !addressRecord.city.length > 0 )
				addressRecord.state = addressParts[2];
		}
		else if( addressParts.length > 2 && addressParts[2].length > 0 )
			addressRecord.state = trim( addressParts[2] );
	}
	else if( addressParts != null && addressParts.length > 0 && addressParts[0].length > 0 )
	{
		zipCheck = addressParts[0].match( numbersOnlyRegExp );
		
		if( zipCheck != null )
			addressRecord.zip = zipCheck.toString();
		else 
		{
			// If the user entered a single first part of an address
			// that was not a zip, let's use it as the city.
			
			// We don't insert a default state here & push that job to the caller of this function
			var words = addressParts[0].split(" ");
			var state = "", city = "", lookup = null;
			var done = false;
			var i = 0;
			
			state = words.pop();
			while (!done && words.length >= 0 && i < 3)
			{
				lookup = gStates[state.toUpperCase()];
				if (lookup != null)
				{
					state = lookup;
					if (words.length > 0)
						city = words.join(" ");
					else
						city = "";
					done = true;
				}
				else if (words.length > 0)
					state = words.pop() + " " + state;
				else
					break;
				i++;
			}
			
			if (!done)
			{
				// we couldn't find a state.
				city = state;
				state = "";
			}

			addressRecord = { address:"", city:city, state:state, zip:"" }; 
		}
	}
	return addressRecord;
}

function trim(strText)
{

    // this will get rid of leading spaces 
    while (strText.substring(0,1) == ' ') 
        strText = strText.substring(1, strText.length);

    // this will get rid of trailing spaces 
    while (strText.substring(strText.length-1,strText.length) == ' ')
        strText = strText.substring(0, strText.length-1);

   return strText;

} 

function URLEncode( unencodedText )
{
	if( unencodedText.length > 0 )
	{	
		var allowedChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.!~*'()";
		var hexcodes = "01256789ABCDEF";
		var space = " ";
	
		var origText = unencodedText;
		var encodedText = "";
		var lastCharWasPlusEncoded = false;
		
		for(var idx = 0; idx < origText.length; idx++ )
		{
			var aChar = origText.charAt(idx);
			
			if( allowedChars.indexOf(aChar) != -1)
			{
				encodedText += aChar;
				lastCharWasPlusEncoded = false;
			}
			else
			{
				var charCode = aChar.charCodeAt(0);
				if ( aChar == space || charCode > 255)
				{
					if( lastCharWasPlusEncoded == false ) // don't build a line of multiple "+" - one should be enough
					{
						encodedText += "+"; // spaces and beyond 8 bit will be discarded and replaced with "+"
						lastCharWasPlusEncoded = true;
					}
				}
				else
				{
					encodedText += ( "%" + hexcodes.charAt((charCode >> 4) & 0xF));
					encodedText += hexcodes.charAt(charCode & 0xF);
					lastCharWasPlusEncoded = false;
				}
			}
		}
	}
	else
		encodedText = unencodedText;
	return encodedText;
};

function updateStatusText( searching, startIdx, endIdx )
{
	var statusText =  "";
	
	if( searching == true )
		statusText = getLocalizedString("Searching...");
	else if( gLineViews.getLength() > 0 )
	{
	// If the view has data, figure out what kind of status line to show
	// else, just blank status line, and we should see "No Results Found" in the results area
		//statusText = ( startIdx + 1 ).toString() + "-" + ( endIdx ).toString();
		statusText = (startIdx + 1).toString() + "–" + endIdx.toString();
		if (gLastSearchData.totalPossibleRecords > 10)
			statusText = statusText + " " + getLocalizedString('of') + " " + gLastSearchData.totalPossibleRecords;
		document.getElementById('statusTextDiv').style["text-align"]="center";
	}
	
	document.getElementById('statusTextDiv').innerText = statusText;
}

function updateArrows(backEnabled, forwardEnabled)
{
	
    var backArrow = document.getElementById('backArrow');
    backArrow.style.visibility="visible";
    
    if( backEnabled )
    {
    	backArrow.setAttribute("enabled", "true" );
    	backArrow.src = 'Images/backarrow_on.png';
    }
    else
    {
    	backArrow.src = 'Images/backarrow_off.png';
    	backArrow.setAttribute("enabled", "false" );
    }
    
    var forwardArrow = document.getElementById('forwardArrow');
    forwardArrow.style.visibility="visible";

    if( forwardEnabled )
    {
	    forwardArrow.setAttribute("enabled", "true");
    	forwardArrow.src = 'Images/forwardarrow_on.png';
    }
    else
    {
    	forwardArrow.src = 'Images/forwardarrow_off.png';
	    forwardArrow.setAttribute("enabled", "false");
    }
}

function noArrows()
{
    document.getElementById('backArrow').style.visibility="hidden";
    document.getElementById('forwardArrow').style.visibility="hidden";
}


function updateScrollbar()
{
	if( gScrollController )
		gScrollController.updateScrollbarForNewView( gPhoneBookMainView.getViewToDocRatio());
}

/*function firstNameCancel()
{
	var fn = document.embeds['firstName-input'];
	fn.setValue("");
	fn.setFocus();
	nameInput();
}*/

function lastNameCancel()
{	
	var ln = document.embeds['lastName-input'];
	var fn = document.embeds['firstName-input'];
	
	if (gResizeAnimation == null)	
	{
		ln.setValue("");
		fn.setValue("");
		fn.setFocus();
		focusFirstName();
	}
	else
	{
		ln.setFocus();
		focusLastName();
	}
	document.embeds['lastName-input'].setCancelImage(0);
	gLastSearchData.firstName = "";
    gLastSearchData.lastName = "";
	gPhoneBookMainView.reset();
	gPhoneBookMainView.growView(false, false);
}

function nameInput()
{
	var fn = getFirstName();
	var ln = getLastName();
	var state;
	if (fn.length > 0 || ln.length > 0)
		state = 1;
	else
		state = 0;
	document.embeds['lastName-input'].setCancelImage(state);
}

function setupWidget()
{
	gCacheData = new CCacheData(); // just the data
	gLineViews = new CLineViews(); // html for the line views
	gPhoneBookMainView = new CPhoneBookMainView();
	gScrollController = new CScroll(159,65);
	
	new AppleInfoButton(document.getElementById('infoButton'), document.getElementById('front'), "black", "black", showbackside);
   	var doneButton = document.getElementById('done-button');
	new AppleGlassButton(doneButton, getLocalizedString('Done'), donePressed,58);

    document.getElementById('bodyDiv').style.visibility="hidden";
    document.getElementById('titleScreen-label').innerText = getLocalizedString('White Pages');

    document.getElementById('itemsperpage-label').innerText = getLocalizedString('Items per page:');
    document.getElementById('searchwithin-label').innerText = getLocalizedString('Search within:');	

	loadPreferences();
	
	var fn = document.embeds['firstName-input'];
	var ln = document.embeds['lastName-input'];
	var cs = document.embeds['citystate-input'];
	
	fn.setNextKeyView(ln);
    ln.setNextKeyView(cs);
    cs.setNextKeyView(fn);
	
	fn.setPlaceholder(getLocalizedString('First Name'));
	ln.setPlaceholder(getLocalizedString('Last Name'));
    cs.setPlaceholder(getLocalizedString('City, State or ZIP Code'));

	fn.setHasSearchIcon(true);
	ln.setHasSearchIcon(false);
	cs.setHasSearchIcon(false);
	
	fn.setOninput(nameInput);
	ln.setOninput(nameInput);
	cs.setOninput(cityStateInput);
	
	fn.setCancelImage(0);
	ln.setCancelImage(0);
	cs.setCancelImage(0);
	
	fn.setOnchange(searchInputChanged);
	ln.setOnchange(searchInputChanged);
	cs.setOnchange(cityStateChanged);
	
	//fn.setOncancel(firstNameCancel);
	fn.setOncancel(lastNameCancel);
	ln.setOncancel(lastNameCancel);
    cs.setOncancel(searchInputCancelled);

	fn.setOnfocus(focusFirstName);
	ln.setOnfocus(focusLastName);
    cs.setOnfocus(focusCityState);

	fn.setOnblur(unfocusFirstName);
	ln.setOnblur(unfocusLastName);
	cs.setOnblur(unfocusCityState);
	
	fn.setEnabled(true);
	ln.setEnabled(true);
	cs.setEnabled(true);
    
    populatePrefSelects();
}

function loadPreferences() {
	if (window.widget)
    {
        pref = widget.preferenceForKey("searchwithin");
        if (pref != null && pref.length > 0)
        {
            gDefaultSearchWithinRange = parseInt(pref);
        }
        pref = widget.preferenceForKey("itemsperpage");
        if (pref != null && pref.length > 0)
        {
            gDefaultItemsPerPageArrayIndex = parseInt(pref);
            gPhoneBookMainView.fDataDisplayResultsPerPage = gItemsPerPageOptions[gDefaultItemsPerPageArrayIndex];
        }
    }
}


function populatePrefSelects()
{
	var select = document.getElementById('searchwithin-popup');
    var c = gSearchWithinOptionsArray.length;

    for ( var i = 0; i < c; ++i )
    {
        var element = document.createElement("option");
        var value = gSearchWithinOptionsArray[i];
        element.innerText = value + getLocalizedString(' Miles');
        element.setAttribute("val", value);
        select.appendChild (element);
    }
    
    select = document.getElementById('itemsperpage-popup');
    var c = gItemsPerPageOptions.length;

    for ( var i = 0; i < c; ++i )
    {
        var element = document.createElement("option");
        var value = gItemsPerPageOptions[i];
        element.innerText = value + getLocalizedString(' items');
        element.setAttribute("val", value);
        select.appendChild (element);
    }
		selectPrefSelects();
}

function selectPrefSelects() {
    document.getElementById("searchwithin-popup").options[gDefaultSearchWithinRange].selected = true;
    document.getElementById("itemsperpage-popup").options[gDefaultItemsPerPageArrayIndex].selected = true;	
}

function createkey(key)
{
	return widget.identifier + "-" + key;
}

function constrainString(string, maxLength)
{
    if ( string.length >= maxLength )
        string = string.substr(0,maxLength) + "...";
        
    return string;
}

function searchWithinPopupChanged (select)
{
    if ( gDefaultSearchWithinRange != select.selectedIndex )
    {
        gDefaultSearchWithinRange = select.selectedIndex;
        
        if (window.widget)
        {
			widget.setPreferenceForKey (gDefaultSearchWithinRange.toString(), "searchwithin");
		}
    }
}

function itemsPerPagePopupChanged (select)
{
    if ( gDefaultItemsPerPageArrayIndex != select.selectedIndex )
    {
        gDefaultItemsPerPageArrayIndex = select.selectedIndex;
        
        if (window.widget)
        {
			widget.setPreferenceForKey(gDefaultItemsPerPageArrayIndex.toString(), "itemsperpage");
		}
    }
}


function clearAllTimers()
{
	clearStatusAnimationTimer();
	if( gResizeAnimation )
	{
		gResizeAnimation.forceEnd(); // force any resize animation to end if it is not done
	}
}

function onhide()
{
	if ( window.widget ) 
    {
    	document.getElementById("highlight-left").style.visibility = 'hidden';
    	document.getElementById("highlight-right").style.visibility = 'hidden';
    	
    	cancelModals();
        if ( document.embeds['citystate-input'] != null )
        	document.embeds['citystate-input'].getSubPlugin().hideLargeType();
        
        //if (window.WhitePagesPlugin)
        //	WhitePagesPlugin.hideLargeType();
        
        // we're hiding the widget, let's see if we have some timers running and kill them if so
        clearAllTimers();
    }
}

function onblur()
{
    if ( window.widget ) 
    {
    	document.getElementById("highlight-left").style.visibility = 'hidden';
    	document.getElementById("highlight-right").style.visibility = 'hidden';
    	
    	//cancelModals();
        if ( document.embeds['citystate-input'] != null )
        	document.embeds['citystate-input'].getSubPlugin().hideLargeType();
        
        //if (window.WhitePagesPlugin)
        //	WhitePagesPlugin.hideLargeType();
        
        // we're hiding the widget, let's see if we have some timers running and kill them if so
        clearAllTimers();
    }
}

function onfocus()
{
    if( window.widget ) 
    {     
        if (gFocusedField == 0)
        {
        	document.embeds['lastName-input'].setFocus();
        	document.getElementById("highlight-right").style.visibility = 'visible';
        	document.getElementById("highlight-left").style.visibility = 'hidden';
        }
        else if (gFocusedField == 1)
        {
        	document.embeds['firstName-input'].setFocus();
        	document.getElementById("highlight-left").style.visibility = 'visible';
        	document.getElementById("highlight-right").style.visibility = 'hidden';
        }
        
        if( !gCustomSearchAddressSet )
        {
            // Tack on an empty string to avoid JavaScript from referencing
            // the variable instead of copying it
            var oldAddress = gDefaultSearchFromAddress + '';

            getUserAddress();

            // Only update if the address actually changed
            if( gDefaultSearchFromAddress == oldAddress )
                return;
            
            // We need to notify the search code that we changed the zip code
            gUpdatedUserAddress = true;
            
            /*if( !gPhoneBookMainView.fFrontSideShowing )
            {
                // Since our flip method will actually set our prefs properly,
                // all we have to do is update the input field
                var cityStateInputField = document.getElementById('citystate-input');
                
                // we prefer to set the zip first, and city state if no zip available
                if( gCurrentSearchFromZip.length > 0 )
                    cityStateInputField.value = gCurrentSearchFromZip;
                else if( gCurrentSearchFromCity.length > 0 )
                    cityStateInputField.value = gCurrentSearchFromCity + ", " + gCurrentSearchFromState;
            }*/
        }
    }

}

function scrollChannelClick(event)
{
	gScrollController.channelClick( event );
}

function mouseDownScrollbar (event)
{
	gScrollController.mouseDown( event );
}

function mouseMoveScrollbar (event)
{
	gScrollController.mouseDrag( event );
}

function mouseWheelMoved ( event )
{
	gScrollController.mouseWheelMoved( event );
}

function mouseUpScrollbar (event)
{
	gScrollController.mouseUp( event );
}

function arrowMouseDown(elem)
{
    if ( elem.id == "backArrow" && elem.getAttribute("enabled") == "true" )
    {
        elem.src = "Images/backarrow_clicked.png";
    }
    else if ( elem.id == "forwardArrow" && elem.getAttribute("enabled") == "true")
    {
        elem.src = "Images/forwardarrow_clicked.png";
    }
}

function arrowClick(elem)
{
	cancelModals();
	
	var rangeLimit = gPhoneBookMainView.getCurrentRangeLimit();
	
    if ( elem.id == "backArrow" && elem.getAttribute("enabled") == "true" )
    {
		gPhoneBookMainView.clearDisplay();
        gPhoneBookMainView.setDisplayIndicesToPrevious();
        gPhoneBookMainView.build();
    }
    else if ( elem.id == "forwardArrow" && elem.getAttribute("enabled") == "true")
    {   
		if( gPhoneBookMainView.hasMoreCachedData() == true )
		{
			if( ( parseFloat( gLineViews.fLargestDistance) == 0 ) && parseFloat( gLineViews.fLargestDistance) < parseFloat( rangeLimit ) )
			{
				gPhoneBookMainView.clearDisplay();
				gPhoneBookMainView.setDisplayIndicesToNext();
				gPhoneBookMainView.build();
        	}
        	else
        	{
        		gPhoneBookMainView.updateArrows();
        	}
        }
        else
        {
        	if( ( parseFloat( gLineViews.fLargestDistance) != 0 ) && parseFloat( gCacheData.fLargestDistance) > parseFloat(rangeLimit) )
        	{
				gPhoneBookMainView.clearDisplay();
				gPhoneBookMainView.setDisplayIndicesToNext();
				gPhoneBookMainView.build();
	    	}
	    	else if( gLastSearchData.totalPossibleRecords == -1 || ( gLastSearchData.RecordsFrom < ( parseInt(gLastSearchData.totalPossibleRecords)-1)) ) // don't search if the last record end index reached the total already
	    	{
				gPhoneBookMainView.clearDisplay();
				var searchData = getNewSearchData(false);
				performSearch( searchData, false);
			}
			else
			{
				gPhoneBookMainView.clearDisplay();
				gPhoneBookMainView.setDisplayIndicesToNext();
				gPhoneBookMainView.build();
			}
        }
    }
}

function limit_3 (a, b, c)
{
    return a < b ? b : (a > c ? c : a);
}

function computeNextFloat (from, to, ease)
{
    return from + (to - from) * ease;
}

function animateFlipper()
{
    var T;
	var ease;
	var time = (new Date).getTime();
		
	T = limit_3(time-animation.starttime, 0, animation.duration);
	
	if (T >= animation.duration)
	{
		clearInterval (animation.timer);
		animation.timer = null;
		animation.positionNow = animation.positionTo;
	}
	else
	{
		ease = 0.5 - (0.5 * Math.cos(Math.PI * T / animation.duration));
		animation.positionNow = computeNextFloat (animation.positionFrom, animation.positionTo, ease);
	}
	
	animation.element.style.opacity = animation.positionNow;
}

function mousemove (event)
{
	if (!flipShown)
	{
		// fade in the flip widget
		if (animation.timer != null)
		{
			clearInterval (animation.timer);
			animation.timer  = null;
		}
		
		var starttime = (new Date).getTime() - 13; // set it back one frame
		
		animation.duration = 500;
		animation.starttime = starttime;
		animation.element = document.getElementById ('flip');
		animation.timer = setInterval ("animateFlipper();", 13);
		animation.positionFrom = animation.positionNow;
		animation.positionTo = 1.0;
		animateFlipper();
		flipShown = true;
	}
}

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
		
		var starttime = (new Date).getTime() - 13; // set it back one frame
		
		animation.duration = 500;
		animation.starttime = starttime;
		animation.element = document.getElementById ('flip');
		animation.timer = setInterval ("animateFlipper();", 13);
		animation.positionFrom = animation.positionNow;
		animation.positionTo = 0.0;
		animateFlipper();
		flipShown = false;
	}
}

function showbackside(event)
{
	cancelModals();
	showHideBitsForFlip( false );
	doFlipToBack();
}

function doFlipToBack()
{
	var front = document.getElementById("front");
	var back = document.getElementById("behind");
	
	//var cityStateInputField = document.getElementById('citystate-input');
	
	if (window.widget)
		widget.prepareForTransition("ToBack");
	
	back.style.visibility="visible";
	
	// before we flip, update user address if we're not using a custom one
	// the me card in user's AB might have changed and we're going to set it
	// into the search field a few lines below here, so be sure to have the latest.
	//if( !gCustomSearchAddressSet )
	//	getUserAddress();

	if (window.widget)
		setTimeout ('widget.performTransition();', 0);

	// we prefer to set the zip first, and city state if no zip available
	//if( gCurrentSearchFromZip.length > 0 )
	//	cityStateInputField.value = gCurrentSearchFromZip;
	//else if( gCurrentSearchFromCity.length > 0 )
	//	cityStateInputField.value = gCurrentSearchFromCity + ", " + gCurrentSearchFromState;

    //cityStateInputField.focus();
}

function donePressed(event)
{
	var back = document.getElementById("behind");
	
	if (window.widget)
		widget.prepareForTransition("ToFront");
		
	back.style.visibility="hidden";
	
	setTimeout ('flipitback();', 0);
}

function showHideBitsForFlip( show )
{
	gPhoneBookMainView.fFrontSideShowing = show;
	
	if( show )
	{
		// clear any animated timers...
		if( gStatusAnimationTimerData != null )
		{
			clearStatusAnimationTimer();
			// the validate timer string needs to be hidden
			document.getElementById('validate').style.visibility="hidden";
		}
		
		document.getElementById('front').style.visibility = "visible";
				
		// set height to the collapsed height value since the widget might have flipped 
		if( gPhoneBookMainView.fIsCollapsed )
		{
			window.resizeTo(335, gConstWidgetCompactHeight);
			document.getElementById("topDiv").style.height = gConstWidgetCompactHeight;
			gScrollController.updateScrollbarForNewView(0);
		}
		else if (gLineViews.getLength() == 0)
			gPhoneBookMainView.showHideTitle( true );
	}
	else
	{		
		// clear any animated timers...
		if( gStatusAnimationTimerData != null )
		{
			clearStatusAnimationTimer();		
			document.getElementById('statusTextDiv').innerText = ""; // don't hide this one as it has multiple uses
		}
		
		gPhoneBookMainView.showHideTitle( false );
		if (gPhoneBookMainView.fIsCollapsed )
			window.resizeTo(336, gConstWidgetCompactHeight);
		else
			window.resizeTo(336, gConstWidgetExpandedHeight);

		document.getElementById('front').style.visibility = 'hidden';
	}
	updateScrollbar();
}

function flipitback()
{
	if (window.widget)
		setTimeout("widget.performTransition();", 0);
	
	// turn off the timer we set to animate the "validating..." string	
	
	// we hide validate element, but other status lines may not hide after completion
		
	showHideBitsForFlip( true ); // show
	
	if( gPhoneBookMainView.preferencesUpdated() ) // only search if prefs changed
		newSearch();
	else
	{
		if (gFocusedField == 0)
		{
			// lastname
			document.embeds['lastName-input'].setFocus();
			focusLastName();
		}
		else if (gFocusedField == 1)
		{
			// first name
			document.embeds['firstName-input'].setFocus();
			focusFirstName();
		}
		else if (gFocusedField == 2)
		{
			// city state
			document.embeds['citystate-input'].setFocus();
			focusCityState();
		}
	}
}

if ( window.widget )
{
    widget.onblur = onblur;
    widget.onhide = onhide;
    widget.onfocus = onfocus;
	widget.onsync = onsync;
    //widget.onremove = onremove;
}

function onsync() {
	loadPreferences();
	updateCityStateInputField();
	selectPrefSelects();
	gPhoneBookMainView.preferencesUpdated();
}

function updateCityStateInputField() {
	// Since our flip method will actually set our prefs properly,
    // all we have to do is update the input field
    var cityStateInputField = document.getElementById('citystate-input');
    
    // we prefer to set the zip first, and city state if no zip available
    if( gCurrentSearchFromZip.length > 0 )
        cityStateInputField.value = gCurrentSearchFromZip;
    else if( gCurrentSearchFromCity.length > 0 )
        cityStateInputField.value = gCurrentSearchFromCity + ", " + gCurrentSearchFromState;
 
}

/*function debug(msg)
{
	if (!debug.box)
	{
		debug.box = document.createElement("div");
		debug.box.setAttribute("style", "background-color: white; " +
										"font-family: monospace; " +
										"border: solid black 3px; " +
										"position: absolute;top:300px;" +
										"padding: 10px;");
		document.body.appendChild(debug.box);
		debug.box.innerHTML = "<h1 style='text-align:center'>Debug Output</h1>";
	}
	
	var p = document.createElement("p");
	p.appendChild(document.createTextNode(msg));
	debug.box.appendChild(p);
}*/

function clickOnProvider(event)
{
    if (window.widget)
        widget.openURL("http://www.daplus.us");
}


function fetchPhoneBookData()
{		
	if( gLastQuery != null )
		performXMLRequest("search", gLastQuery, dataLoaded);
}

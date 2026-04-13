/*
Copyright (c) 2005, Apple Computer, Inc.  All rights reserved.
NOTE:  Use of this source code is subject to the terms of the Software
License Agreement for Mac OS X, which accompanies the code.  Your use
of this source code signifies your agreement to such license terms and
conditions.  Except as expressly granted in the Software License Agreement
for Mac OS X, no other copyright, patent, or other intellectual property
license or right is granted, either expressly or by implication, by Apple.
*/

var ChineseS = ['en'];
var ChineseT = ['en'];
var Dutch = ['en','fr'];
var English = [
    'zh-Hans',
    'zh-Hant',
    'nl',
    'fr',
    'de',
    'el',
    'it',
    'ja',
    'ko',
    'pt',
    'ru',
    'es'
];
var French = ['nl','en','de','el','it','pt','es'];
var German = ['en','fr'];
var Greek = ['en','fr'];
var Italian = ['en','fr'];
var Japanese = ['en'];
var Korean = ['en'];
var Portuguese = ['en','fr'];
var Russian = ['en'];
var Spanish = ['en', 'fr'];

var languageData = [
        {name:'Chinese (Simplified)', code:"zh", iso:"zh-Hans", array:ChineseS},
        {name:'Chinese (Traditional)', code:"zt", iso:"zh-Hant", array:ChineseT},
        {name:'Dutch', code:"nl", iso:"nl", array:Dutch},
        {name:'English', code:"en", iso:"en", array:English},
        {name:'French', code:"fr", iso:"fr", array:French},
        {name:'German', code:"de", iso:"de", array:German},
        {name:'Greek', code:"el", iso:"el", array:Greek},
        {name:'Italian', code:"it", iso:"it", array:Italian},
        {name:'Japanese', code:"ja", iso:"ja", array:Japanese},
        {name:'Korean', code:"ko", iso:"ko", array:Korean},
        {name:'Portuguese', code:"pt", iso:"pt", array:Portuguese},
        {name:'Russian', code:"ru", iso:"ru", array:Russian},
        {name:'Spanish', code:"es", iso:"es", array:Spanish}
];

var gDefaultFromLanguageISO = "en"; // use English as the default from language (index 3?)
var gDefaultToLanguageISO = "fr"; // use French as the default to language (index 4?)

// gAllowTextToGrow is used to determine when we will let the user add 
//	to the "from" text
// Once either the "to" or the "from" text would require more than six
//	lines, we stop accepting additions to the "from" text
var gAllowTextToGrow = true;

var kLineWidth = 291;
var kMaxNumLines = 6;
var kMinNumLines = 3;
var kDefaultLineHeight = 18;
var kMinTextBoxHeight = kMinNumLines * kDefaultLineHeight;
var kDefaultTextSize = 15;

var kAbsoluteMaximumWidgetHeight = 750;
var kDefaultWidgetWidth = 328;
var kDefaultWidgetHeight = 240

var kAnimationFrameMilliseconds = 13;
var kAnimationDuration = 250;

var fromLanguageFont = "Helvetica Neue";
var fromLanguageSize = kDefaultTextSize;
var fromLanguageLineHeight = kDefaultLineHeight;
var toLanguageFont = "Helvetica Neue";
var toLanguageSize = kDefaultTextSize;
var toLanguageLineHeight = kDefaultLineHeight;
var kFromLanguageKey = "fromLanguage";
var kToLanguageKey = "toLanguage";

var requestID = 0;
var receiveID = 0;

var timerInterval = null;

var lastEncodedBlock = '';

var gLastTranslationXMLRequest = null;

var gTranslationTimeoutID;
var kTranslationDelay = 2000;

if (window.widget) {
	widget.onshow = onshow;
	widget.onsync = onsync;
	widget.onremove = onremove;
}

function onshow() {
	resizeFromTextAsNeeded();
    gFromText.focus();
}

function onsync() {
	fetchPreferenceDataAndUpdateLanguagePopupMenus();
}

function onremove() {
	setInstancePreferenceForKey(null, kFromLanguageKey);
	setInstancePreferenceForKey(null, kToLanguageKey);
}

function $(id) {
	return document.getElementById(id);
}

function createKey(key) {
	if (window.widget) {
        key = widget.identifier + "-" + key;
	}
	return key;
}

function setInstanceAndGlobalPreferenceForKey(value, key) {
	setInstancePreferenceForKey(value, key);
	setGlobalPreferenceForKey(value, key);
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
	var result = null;
	if (window.widget) {
		result = widget.preferenceForKey(createKey(key));
		if (!result) {
			result = widget.preferenceForKey(key);
		}
	}
	return result;
}

function doBeep() {
	if (!gAllowTextToGrow && TranslationEncoder != null) TranslationEncoder.beep();
}

function languageDataFromISO(iso) {
	for (var i = 0; i < languageData.length; i++)
		if (languageData[i].iso == iso)
			return languageData[i];
	return null;
}

function languageDataIndexForISO(iso) {
	for (var i = 0; i < languageData.length; i++)
		if (languageData[i].iso == iso)
			return i;
	return 0;
}

function supportedLanguagesArrayContainsISO(array, iso) {
	if (array != null)
		for (var i = 0; i < array.length; i++) {
			if (array[i] == iso)
				return true;
		}
	return false;
}

function fontForLanguage(language) {
	var returnValue = "Helvetica Neue";
	if (language == "el" || language == "ru")
		returnValue = "Helvetica";
	else if (language == "ja")
		returnValue = "Hiragino Maru Gothic Pro";
	else if (language == "ko")
		returnValue = "AppleGothic";
	else if (language == "zh-Hans")
		returnValue = "STHeiti";
	else if (language == "zh-Hant")
		returnValue = "LiHei Pro";
	return returnValue;
}

function lineHeightForFont(fontName) {
	if (fontName == "Helvetica Neue")
		return 18;
	else if (fontName == "Helvetica")
		return 19;
	else if (fontName == "Hiragino Maru Gothic Pro")
		return 23;
	else if (fontName == "AppleGothic")
		return 18;
	else if (fontName == "STHeiti")
		return 16;
	else if (fontName == "LiHei Pro")
		return 16;
	else return 18;		/* We should never get here */
}

var gFromText;
var gToText;

var gBGImage = null;
var gGlobeBodyImage = null;
var gGlobeTextFieldImage = null;


function load() {
	/*
		A STATEMENT ABOUT THE PREFERENCE SAVED FOR FROM AND TO LANGUAGE DATA:
	
		We used to use a global index to point to a given item in our languageData array.
		
		This global index was used extensively throughout the code.
		
		We are moving away from using that global index to use the language ISO code instead.
		
		The language ISO code is not as strongly coupled to the position of data in the array.
		You can search the languageData for an item that has the ISO code (which should be a
		unique key used only by one languageData array item...) and retrieve it regardless of
		changes in size or order of the languageData array.
		
		However, for now we are still saving the old index value to preferences, so that we don't
		have to create a new preference key, and all that implies...
		
		But we will immediately convert the index from preferences to an ISO code on loading the
		preference, and we convert our global ISO values back to a indicies when saving preferences.
		
		At some point, we should abandon using the index as a pref and just use the ISO, but we
		will need to be careful so that users who have the old index pref saved can make a smooth
		transition.
	*/
	
	findElements();
	fetchPreferenceDataAndUpdateLanguagePopupMenus();

    $('fromLabel').innerText = getLocalizedString('Translate from');
    gFromText.value = "";
    gToText.value = "";
    $('unavailableLabel').innerText = "";
    $('toLabel').innerText = getLocalizedString('To');
    gAllowTextToGrow = true;
    
	//gFromText.addEventListener("cut", translateCut, false);
	gFromText.addEventListener("paste", translatePaste, false);
    
    focusFromText();
    
	
    // Set up back but do not display
	var doneButton = $('done');
	new AppleGlassButton(doneButton, getLocalizedString('Done'), selectDone);
	
	//set up info button
	new AppleInfoButton($('infoButton'), $('front'), "white", "white", showbackside);
	
	// Set up swap button
	var swapButton = $('swapButton');

	var swapButtonWidth  = 32;
	var swapButtonHeight = 25;
	var b = new AppleButton(swapButton, "", swapButtonHeight, //element, text, height
					null, null, 0, //imgLeft
					'Images/translator_swap_OFF.png', 'Images/translator_swap_ON.png', //imgMiddle
					null, null, 0, //imgRight
					switchLanguages);
	b.textElement.style.width = swapButtonWidth + "px";
}

function fetchPreferenceDataAndUpdateLanguagePopupMenus() {
	if (window.widget) {
        var userLanguages = TranslationEncoder.userLanguagesByPreference();
        var userLangIdx = 0;

		// what language will we be translating from?
		
		var pref = parseInt(preferenceForKey("fromLanguage"));  //check user's saved preferences first

		var fromIndex;
		if (pref !== null && pref !== undefined && !isNaN(pref) && pref >= 0) { // easy case
			// Make sure the pref index isn't out of bounds
			fromIndex = (pref < languageData.length) ? pref : 0;
			gDefaultFromLanguageISO = languageData[fromIndex].iso;

		} else { // no pref for "from language" was found
			//we need to set up the "from" language menus based on the user's current preferred language
			
			var foundLanguage = false;

			// walk the user's preferred languages (specified in International Prefs Pane) until we find a language
			// in the user's language list which we support (i.e., it is in our languageData)

			for (userLangIdx = 0; userLangIdx < userLanguages.length && !foundLanguage; userLangIdx++) {
				for (var i = 0; i < languageData.length; i++) {
					if (TranslationEncoder.localizedNameForLanguage(languageData[i].iso) == userLanguages[userLangIdx]) {
						foundLanguage = true;
						break;
					}
				}
			}
			fromIndex = i;
			gDefaultFromLanguageISO = languageData[fromIndex].iso;
					
		}

		// what language will we be translating to?
		
		var pref = parseInt(preferenceForKey("toLanguage"));  // check user's saved preferences first
		
		var toIndex;
		if (pref !== null && pref !== undefined && !isNaN(pref) && pref >= 0) { // easy case
			var fromLangData = languageDataFromISO(gDefaultFromLanguageISO);
			
			// Make sure the pref index isn't out of bounds
			toIndex = (pref < fromLangData.array.length) ? pref : 0;
			gDefaultToLanguageISO = fromLangData.array[toIndex];
				
			
		} else { // no pref for "to language" was found
			// we need to set up the "to" language menus based on what is supported by the "from language"
			
			var foundLanguage = false;
			
			// walk the user's preferred languages (specified in International Prefs Pane) until we find a language
			// in the user's language list which we support (i.e., it is in our languageData)
			
			var fromLangData = languageDataFromISO(gDefaultFromLanguageISO);
			
			// assertion: fromLangData always has array by data definition at head of this file
			if (fromLangData != null) {
				var supportedToLanguagesArray = fromLangData.array;
				var j = 0;
				
				for (; userLangIdx < userLanguages.length && !foundLanguage; userLangIdx++) {
					for (var j = 0; j < supportedToLanguagesArray.length; j++) {
						if (TranslationEncoder.localizedNameForLanguage(supportedToLanguagesArray[j]) == userLanguages[userLangIdx]) {
							foundLanguage = true;
							break;
						}
					}
				}
				toIndex = j;
				gDefaultToLanguageISO = supportedToLanguagesArray[j];
			}
		}

		// save the index of the language that matched a language in user's languages to preferences
		setInstanceAndGlobalPreferenceForKey(""+fromIndex, kFromLanguageKey);
		setInstanceAndGlobalPreferenceForKey(""+toIndex, kToLanguageKey);

	} else { // no window.widget
		gDefaultFromLanguageISO = languageData[3].iso;
		gDefaultToLanguageISO = languageData[3].iso;
	}
    setupLanguageMenusForDefaults();
	syncFromLanguageAndFont();
	syncToLanguageAndFont();
}

function setupLanguageMenusForDefaults() {
	populateLanguageSelect(false); // don't try to match last current language since this is the initial call

	if (window.widget) {
		$('fromStaticLabel').innerText = TranslationEncoder.localizedNameForLanguage(gDefaultFromLanguageISO);
		var supportedLanguages = languageDataFromISO(gDefaultFromLanguageISO).array;
		$('toStaticLabel').innerText = TranslationEncoder.localizedNameForLanguage(gDefaultToLanguageISO);    
	} else 
	{
		$('fromStaticLabel').innerText = getLocalizedString(languageDataFromISO(gDefaultFromLanguageISO).name);
		$('toStaticLabel').innerText = getLocalizedString(gDefaultToLanguageISO);
	}

	//walk the from language menu options and select the one that matches the default ISO
	var fromLangMenu = $ ('fromPopup');
	
	for (var i = 0; i < fromLangMenu.options.length; i++)
		if (fromLangMenu.options[i].getAttribute("iso") == gDefaultFromLanguageISO)
			fromLangMenu.options[i].selected= true;
	
	//walk the to language menu options and select the one that matches the default ISO
	var toLangMenu = $ ('toPopup');
	
	for (var i = 0; i < toLangMenu.options.length; i++)
		if (toLangMenu.options[i].getAttribute("iso") == gDefaultToLanguageISO)
			toLangMenu.options[i].selected= true;
}

function getLocalizedString (key) {
	var returnValue = key;
	try {
		// This handles the case where localizedStrings[key] doesn't exist
		if (localizedStrings[key] != null)
			returnValue = localizedStrings[key];
	} catch (ex) {
		// This handles the case where localizedStrings[] doesn't exist
		alert(key);
	}
	return returnValue;
}

function getTranslationType() {
	var fromLangData = languageDataFromISO(gDefaultFromLanguageISO);

	// The translation server likes strings like en_ja to translate from english
	//  to japanese, here's where we create the string
	var fromLang = languageDataFromISO(gDefaultFromLanguageISO);
	var toLangData = languageDataFromISO(gDefaultToLanguageISO);

	return fromLang.code + "_" + toLangData.code;
}

function getEncodingType(code) {
	// if (code == "ja") return "utf-8";
	// else if (code == "zh") return "EUC-CN";
	// else if (code == "zt") return "BIG5";
	// else if (code == "ko") return "EUC-KR";
	// else if (code == "ru") return "WINDOWS-1251";
	// else if (code == "el") return "ISO-8859-7";
	// else

	//see 7125777, just use utf-8 now. 
	return "utf-8";
}

function findElements() {
	gFromText = $("fromTextArea");
	gFromText.fLastLineCount = kMinNumLines;
	gToText = $("toTextArea");
}

function syncFromLanguageAndFont() {
	fromLanguageFont = fontForLanguage(gDefaultFromLanguageISO);
	fromLanguageLineHeight = lineHeightForFont(fromLanguageFont);
	gFromText.style.fontFamily = fromLanguageFont;
}

function resizeFromTextAsNeeded() {
	syncFromLanguageAndFont();
	syncToLanguageAndFont();
	
	var encodedText = gFromText.value;
	var textToTranslate = encodedText;
	var numLines = kMinNumLines;
	if (window.widget) {
		if (window.TranslationEncoder) {
			var fromLines = TranslationEncoder.linesNeededToFitText(textToTranslate, fromLanguageFont, fromLanguageSize, kLineWidth);
			var toLines = TranslationEncoder.linesNeededToFitText(textToTranslate, toLanguageFont, toLanguageSize, kLineWidth);
			numLines = (fromLines > toLines) ? fromLines : toLines;
			// enforce a maximum number of lines by trimming text
			if (numLines > kMaxNumLines) {
				textToTranslate = TranslationEncoder.textTrimmedToFitLineCount(textToTranslate, fromLanguageFont, fromLanguageSize, kLineWidth, kMaxNumLines);
				numLines = kMaxNumLines;
				gAllowTextToGrow = false;
				doBeep();
			} else if (numLines < kMinNumLines)	{
				numLines = kMinNumLines;
			}
		} else {
			alert("TranslationEncoder not found.  Problem with Translation.widgetplugin?");
		}
	}
	gFromText.value = new String(textToTranslate);

	if (numLines != gFromText.fLastLineCount) {
		doResizeWidget(numLines);
	}
	
	gFromText.fLastTextLen = encodedText.length;
	gFromText.fLastLineCount = numLines;
}

function doResizeWidget(lines) {
	if (lines == gFromText.fLastLineCount) { // already correct height
		return;
	}
	
	var lineDiff = lines - kMinNumLines;
	var fromLineHeight = lineHeightForFont(fromLanguageFont);
	var toLineHeight = lineHeightForFont(toLanguageFont);
	var lineHeight = (fromLineHeight > toLineHeight) ? fromLineHeight : toLineHeight;
	var heightDiff = (lineDiff * 2) * lineHeight;
	var newHeight = kDefaultWidgetHeight + heightDiff;
	newHeight = (newHeight > kAbsoluteMaximumWidgetHeight) ? kAbsoluteMaximumWidgetHeight : newHeight;
	
	var startRect = new AppleRect(0, 0, kDefaultWidgetWidth, window.innerHeight);
	var finRect = new AppleRect(0, 0, kDefaultWidgetWidth, newHeight);
	
	var animator = new AppleAnimator(kAnimationDuration, kAnimationFrameMilliseconds);
	var animation = new AppleRectAnimation(startRect, finRect, widgetResizeHandler);
	animator.addAnimation(animation);
	animator.oncomplete = widgetResizeCompleteHandler;
	animator.start();
}

function widgetResizeHandler(animation, current, start, finish) {
	if (window.widget) {
		widget.resizeAndMoveTo(window.screenX, window.screenY, current.right, current.bottom);
	}
	window.innerHeight = current.bottom;
}

function widgetResizeCompleteHandler() {
	
}

var curNumLines = 2;

function translateTokenizeText(text) {
	var token, tokens, filteredTokens = new Array();
	// Trim the text (note, we use [ \t] here instead of \s
	// because we want line-breaks to be significant [4029701]
	text = text.replace(/^[ \t]*|[ \t]*$/g,"");

	tokens = text.split(/[ \t]/);
	for (var i = 0; i < tokens.length; i++)
		if (tokens[i] != '')
			filteredTokens[filteredTokens.length] = tokens[i];

	return filteredTokens;
}

function compareArrays(array1, array2) {
// We don't need a fully generalized array matching routine here because
// we know we're dealing with two arrays of strings
	if (array1.length != array2.length) {
		return false;
	}
	for (var i=0; i<array1.length; i++) {
		if (array1[i] != array2[i]) {
			return false;
		}
	}
	return true;
}

function consumeEvent(evt) {
	evt.preventDefault();
	evt.stopPropagation();
}

function translateCut(evt) {
	gAllowTextToGrow = true;
	translateText(); 
}

function translatePaste(evt) {
	var str = evt.clipboardData.getData("text/plain");
	consumeEvent(evt);
	
	var start = gFromText.selectionStart;
	var end = gFromText.selectionEnd;
	var oldStr = gFromText.value;
	gFromText.value = oldStr.substring(0, start) + str + oldStr.substring(end);
	gFromText.selectionStart = start + str.length;
	
	translateText();
}

function translateKeyPress(evt, sender) {
	//	Special-case backspace (U+0008), tab (U+0009), and characters
	//		in the U+F7xx range of the private use area, which
	//		are used by cursor movement and related keys.  We
	//		also pass through anything with the command key pressed.
	//	These never lengthen the text
	//	N.B., if the user happens to use this part of Unicode's private
	//		use area for data interchange -- itself unlikely -- this
	//		code as written will discard it.  However, the data would
	//		be garbage to Systran, anyway, and not valid input for
	//		its translation service, so discarding it doesn't hurt
	if (evt.charCode == 0x0008 || evt.charCode == 0x0009 || evt.metaKey ||
			(0xF700 <= evt.charCode && evt.charCode <= 0xF7FF))
		processKeystroke(evt, sender);
	else if (evt.charCode == 13 && !evt.altKey) { //if they hit return, send the request right away
		clearTimeout(gTranslationTimeoutID);
		translateText();
		consumeEvent(evt);
	}
	else
		checkText(evt, sender, String.fromCharCode(evt.charCode));
}

function translateKeyUp(evt, sender) {
	clearTimeout(gTranslationTimeoutID);
	gTranslationTimeoutID = setTimeout("translateText();", kTranslationDelay);
}

function processKeystroke(evt, sender) {
	// Backward and forward delete change the text
	if (evt.charCode == 8 || evt.charCode == 63272) {
		gAllowTextToGrow = true;
	}
}

function checkText(evt, sender, text) {
// This function checks to see if the text will put us over our
// six-line input limit; if so, we throw the event away so that
// the text area doesn't even get to add it

	// If there's no text to process, we don't need to do anything but pass
	// the event on
	if (text.length == 0)
		return;

	// If text addition has been blocked, then just kill the event and
	// abort
	if (!gAllowTextToGrow) {
		doBeep();
		consumeEvent(evt);
		return;
	}

	// There's text to add and we're not blocking text additions
	// See if this puts us over the limit
	var encodedText = sender.value + text;
	var numLines = TranslationEncoder.linesNeededToFitText(encodedText, fromLanguageFont, fromLanguageSize, kLineWidth);
	if (numLines > kMaxNumLines) {
		gAllowTextToGrow = (evt.charCode == 10 || evt.charCode == 13);
		doBeep();
		consumeEvent(evt);
	}
}

function translateText() {
	var encodedText = gFromText.value;
	// don't do any work if there is no text
	if (encodedText.length > 0) {
		fetchTranslatedData();
	} else {
		var toText = gToText.value;
		if (toText.length > 0) {
			clearTranslationText();
		}
	}
	resizeFromTextAsNeeded();
}


function fetchTranslatedData() {
	var encodedText = gFromText.value;

	if (encodedText.length > 0) {
		if (gLastTranslationXMLRequest != null)
			gLastTranslationXMLRequest.abort();

		var fromLangData = languageDataFromISO(gDefaultFromLanguageISO);
		
		var encodingType = getEncodingType(fromLangData.code);

		if (window.widget && encodingType != "utf-8")
		 	encodedText = TranslationEncoder.encodedStringForString(encodedText,encodingType);
		
		encodedText = encodedText.replace(/;/, "%3B");

		gLastTranslationXMLRequest = performXMLRequest(encodedText, encodingType, getTranslationType(), translationLoaded);
	}
}


function clearTranslationText() {
	$('unavailableLabel').innerText = "";
	gToText.value = "";
	gAllowTextToGrow = true;
}

function translationLoaded(data, errorString) {
	gLastTranslationXMLRequest = null;

	var encodedText = gFromText.value;
	syncToLanguageAndFont();

	if (encodedText.length > 0) {
		if (data != null && data.length > 0) {
			if (!encodedText.match(/\n/))
				data = data.replace(/\n/g, ' ');
			data = data.replace(/\s+$/, '');
			
			var numLines = kMinNumLines;
			var translatedText = String(data);
			
			if (window.widget) {
				if (TranslationEncoder != null) {
					numLines = TranslationEncoder.linesNeededToFitText(translatedText, toLanguageFont, toLanguageSize, kLineWidth);
					if (numLines < kMinNumLines) {
						numLines = kMinNumLines;
					} else if (numLines > kMaxNumLines) {
						translatedText = TranslationEncoder.textTrimmedToFitLineCount(translatedText, toLanguageFont, toLanguageSize, kLineWidth, kMaxNumLines);
						numLines = kMaxNumLines;
						gAllowTextToGrow = false;
					}
				} else {
					alert("TranslationEncoder not found.  Problem with Translation.widgetplugin?");
				}
			}

			gToText.value = translatedText;
			$('unavailableLabel').innerText = "";
		} else {
			clearTranslationText();
			$('unavailableLabel').innerText = getLocalizedString('Data unavailable.');
		}
	} else {
		clearTranslationText();
	}
}

// this function sorts an Array of data which is used to build the
// from and to language popup menus
function sortLocalizedMenuDataArray(item1, item2) {
	if (item1.locName < item2.locName)
		return -1;
	return 1;
}

function populateLanguageSelect(enforceCurrLanguage) {
	// populate the fromLanguage select
	var select = $('fromPopup');

	var localizedMenuData = new Array();
	var locData = null;

	for (var i = 0; i < languageData.length; ++i) {
		if (window.widget)
			locData = { locName:TranslationEncoder.localizedNameForLanguage(languageData[i].iso), iso:languageData[i].iso };
		else
			locData = { locName:getLocalizedString(languageData[i].name), iso:languageData[i].iso };

		var element = document.createElement("option");

		if (locData != null)
			localizedMenuData.push(locData);
	}
	localizedMenuData.sort(sortLocalizedMenuDataArray);

	for (j = 0; j < localizedMenuData.length; j++) {
		var element = document.createElement("option");
		element.innerText = localizedMenuData[j].locName;
		element.setAttribute("iso", localizedMenuData[j].iso);
		select.appendChild(element);
	}
	populateToLanguageSelect(enforceCurrLanguage);
}

function populateToLanguageSelect(enforceCurrLanguage) {
	// remove all children
	var select = $('toPopup');
	while (select.hasChildNodes()) 
		select.removeChild(select.firstChild);

	// translate the languages for the popup and add them

	var fromLangData = languageDataFromISO(gDefaultFromLanguageISO);
	var toLangData = fromLangData.array;
	
	var localizedMenuData = new Array();
	var locData = null;

	for (var i = 0; i < toLangData.length; ++i) {
		if (window.widget)
			locData = { locName:TranslationEncoder.localizedNameForLanguage(toLangData[i]), iso:toLangData[i] };
		else
			locData = { locName:getLocalizedString(toLangData[i]), iso:toLangData[i] };

		var element = document.createElement("option");

		if (locData != null)
			localizedMenuData.push(locData);
	}
	
	localizedMenuData.sort(sortLocalizedMenuDataArray);
	
	for (var j = 0; j < localizedMenuData.length; j++) {
		var element = document.createElement("option");
		element.innerText = localizedMenuData[j].locName;
		element.setAttribute("iso", localizedMenuData[j].iso);
		select.appendChild(element);
	}

	if (enforceCurrLanguage == true) {
		// try to find the old toLanguage to keep things consistent for the user
		// so, when the fromLanguage is changed, the same toLanguage (if available) is also set

		for (i = 0; i < select.childNodes.length; i++) {
		   if (select.childNodes[i].getAttribute("iso") == gDefaultToLanguageISO) {
				select.options[i].selected = true;
				break;
		   }
		}
	}
}

function fromLanguageChanged (select,smartSelectToLang) {
	var translateElem = gFromText;
	var translationElem = gToText;

	// set the UI
	$('unavailableLabel').innerText = "";

	var currSelectedOption = select.childNodes[select.selectedIndex];
	
	if (window.widget)
		$('fromStaticLabel').innerText = TranslationEncoder.localizedNameForLanguage(currSelectedOption.getAttribute("iso"));
	else
		$('fromStaticLabel').innerText = getLocalizedString(languageDataFromISO(currSelectedOption.getAttribute("iso")).name);

	gDefaultFromLanguageISO = currSelectedOption.getAttribute("iso");
	focusFromText();
	clearTranslationText();
	resizeFromTextAsNeeded();
            
    // with this fromLanguage, repopulate the toLanguage select, try to find the last toLanguage
    populateToLanguageSelect(smartSelectToLang);
    // perform the new translation given the new from/to languages
    toLanguageChanged($('toPopup'));
    
    syncFromLanguageAndFont();   
}

function handleFromLanguageChanged(select) {
	// blank the text area, to prevent mismatch between language of content, and language selected in menu
	gFromText.value = "";
    gAllowTextToGrow = true;
	
	fromLanguageChanged(select,true); // destination text area will be blanked in here
	
	var currSelectedOption = select.childNodes[select.selectedIndex];
	var fromISOIndex = languageDataIndexForISO(currSelectedOption.getAttribute("iso"));
	
	var fromLangData = languageDataFromISO(currSelectedOption.getAttribute("iso"));
	
	var toISOIndex = 0;
	for (var i = 0; i < fromLangData.array.length; i++)
		if (fromLangData.array[i] == gDefaultToLanguageISO)
			toISOIndex = i;
    
    // save to preferences, check that strings arn't undefined before saving 
	// assignment in if () is on purpose
    var toString, fromString;
    if ((toString = fromISOIndex.toString()) && (fromString = toISOIndex.toString())) 
    {
        setInstanceAndGlobalPreferenceForKey(toString, kFromLanguageKey);
        setInstanceAndGlobalPreferenceForKey(fromString, kToLanguageKey);
    }
}

function syncToLanguageAndFont() {
    toLanguageFont = fontForLanguage(gDefaultToLanguageISO);
    toLanguageLineHeight = lineHeightForFont(toLanguageFont);
    gToText.style.fontFamily = toLanguageFont;
}

function toLanguageChanged (select) {   
    var currSelectedOption = select.childNodes[select.selectedIndex];
    
    if (window.widget)
        $('toStaticLabel').innerText = TranslationEncoder.localizedNameForLanguage(currSelectedOption.getAttribute("iso"));
    else
        $('toStaticLabel').innerText = getLocalizedString(currSelectedOption.getAttribute("iso"));

    gDefaultToLanguageISO = currSelectedOption.getAttribute("iso");
	focusFromText();
	
	// Retranslate
	clearTranslationText();
    translateText();

    // reset the UI
    syncToLanguageAndFont();
    
}

function handleToLanguageChanged(select) {
	toLanguageChanged(select);
    
    // set the prefs to the most recent toLanguage
    if (window.widget) {
		var currSelectedOption = select.childNodes[select.selectedIndex];
		
		var fromLangData = languageDataFromISO(gDefaultFromLanguageISO);
		var toISOIndex = 0;
		
		// if the supported language array for fromLangData contains the
		// current "to" iso, use the index of that item for the perference
		// to save (but see note on preferences in setupWidget() above...)
		for (var i = 0; i < fromLangData.array.length; i++)
			if (fromLangData.array[i] == currSelectedOption.getAttribute("iso"))
				toISOIndex = i;

		var toString;
		if (toString = toISOIndex.toString()) { //check that its not undefined
			setInstanceAndGlobalPreferenceForKey(toString, kToLanguageKey);
		}
    }
}

function switchLanguages() {
	if (xmlRequestAwaitingReply())
		return;
		
	var fromIndex = -1, toIndex = -1;

	var fromSelect = $('fromPopup');
	var oldFromLanguageISO = fromSelect.childNodes[fromSelect.selectedIndex].getAttribute("iso");
	
	//alert("old from language ISO: " + oldFromLanguageISO);
	
	var toSelect = $('toPopup');
	var oldToLanguageISO = toSelect.childNodes[toSelect.selectedIndex].getAttribute("iso");
	
	//alert("old to language ISO: " + oldToLanguageISO);
    
    // find a language data struct in our languageData that matches the to language ISO
	var fromLangData = languageDataFromISO(oldToLanguageISO);
    
    // we are swapping the "from" and "to" languages.
    // First, we must find a language data struct in our main languageData list that supports
    // what was the former "to" language. That language data struct has a list of supported
    // target "to" languages (in "array"), and that list must support  what was the former "from" language.
    // Make sense?
	if (fromLangData != null && supportedLanguagesArrayContainsISO(fromLangData.array, oldFromLanguageISO)) {
		// swap the text fields
        var translationText = gToText.value;
        gToText.value = gFromText.value;
        gFromText.value = translationText;
        
        var fromLanguageName = null;
        var toLanguageName = null;
            
        if (window.widget) {
            fromLanguageName = TranslationEncoder.localizedNameForLanguage(oldToLanguageISO);
            toLanguageName = TranslationEncoder.localizedNameForLanguage(oldFromLanguageISO);
        } else {
        	fromLanguageName = getLocalizedString(oldToLanguageISO);
        	toLanguageName = getLocalizedString(oldFromLanguageISO);
        }
        
        var fromSelect = $ ('fromPopup');
        
        $('fromStaticLabel').innerText = fromLanguageName;
        $('toStaticLabel').innerText = toLanguageName;
		
		var fromISOIndex;
		for (var i = 0; i < fromSelect.childNodes.length; i++) {
		   if (fromSelect.childNodes[i].getAttribute("iso") == oldToLanguageISO) {
				fromSelect.options[i].selected = true;
				fromISOIndex = i;
				break;
		   }
		}
		gDefaultFromLanguageISO = oldToLanguageISO;
		gDefaultToLanguageISO = oldFromLanguageISO;
		
		fromLanguageChanged(fromSelect,false);

        var toSelect = $ ('toPopup');

		var toISOIndex;
		for (i = 0; i < toSelect.childNodes.length; i++) {
		   if (toSelect.childNodes[i].getAttribute("iso") == oldFromLanguageISO) {
				toSelect.options[i].selected = true;
				toISOIndex = i;
				break;
		   }
		}
		
	    var toString, fromString;
	    if ((toString = fromISOIndex.toString()) && (fromString = toISOIndex.toString())) 
	    {
	        setInstanceAndGlobalPreferenceForKey(toString, kFromLanguageKey);
	        setInstanceAndGlobalPreferenceForKey(fromString, kToLanguageKey);
	    }
		
		toLanguageChanged(toSelect);
	    translateText();
    }
}

function xmlRequestAwaitingReply() {
	return(gLastTranslationXMLRequest != null);
}

function clickOnProvider(event) {
	if (window.widget) {
	    widget.openURL("http://www.systransoft.com");
	} else {
		window.location = "http://www.systransoft.com";
	}
}

function hideElement(el) {
	el.style.display = "none";
}

function showElement(el) {
	el.style.display = "block";
}

function showbackside(event) {
		var front = $("front");
		var back = $("back");

		if (window.widget)
			widget.prepareForTransition("ToBack");
	
		hideElement(front);
		showElement(back);
	
		if (window.widget)
			setTimeout('widget.performTransition();', 0);
}

function selectDone () {
	var front = $("front");
	var back = $("back");

	if (window.widget)
		widget.prepareForTransition("ToFront");
	
	showElement(front)
	hideElement(back)
	
	if (window.widget)
		setTimeout('widget.performTransition();', 0);
				
	onshow();
}

function focusFromText() {
	gFromText.focus();
}

function focusToText() {
	gToText.focus();
}

function blurText() {
	gToText.blur();
	gFromText.blur();
}

function clearLastXMLRequest() {
	gLastTranslationXMLRequest = null;
}

function isXMLRequestLastOneSent(xml_request) {
    return(xml_request == gLastTranslationXMLRequest);
}


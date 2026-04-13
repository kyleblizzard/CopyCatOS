/*
 Copyright 2005, Apple Computer, Inc.  All rights reserved.
 NOTE:  Use of this source code is subject to the terms of the Software
 License Agreement for Mac OS X, which accompanies the code.  Your use
 of this source code signifies your agreement to such license terms and
 conditions.  Except as expressly granted in the Software License Agreement
 for Mac OS X, no other copyright, patent, or other intellectual property
 license or right is granted, either expressly or by implication, by Apple.
 */

var backsideWidth = 366;
var backsideHeight = 254;
var thumbnailLoadCount = 0;

var WebClipTheme = {
WebClipGlass: 0,
WebClipBlackEdge: 1,
WebClipVintageCorners: 2,
WebClipDeckled: 3,
WebClipPegboard: 4,
WebClipTornEdge: 5
}

var numberOfThemes = 6;

var flipShown = false;
var fadeAnimation = {startTime:0, duration:250, fadeFrom:0, fadeTo:0, fadeNow:0, timer:null, element:null};

function openSafari()
{
    widget.openApplication("com.apple.safari");
}

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

function audio_onclick(element)
{
    var enabled = webClip.playAudioOutOfDashboard();   
    document.getElementById("audioCheckbox").checked = enabled;
    webClip.setPlayAudioOutOfDashboard(!enabled);
}

function tornEdgeIcon_onDblClick(element)
{
    tornEdgeIcon_onclick(element);
    donePressed();
}

function glassIcon_onDblClick(element)
{
    glassIcon_onclick(element);
    donePressed();
}

function deckledIcon_onDblClick(element)
{
    deckledIcon_onclick(element);
    donePressed();
}

function blackEdgeIcon_onDblClick(element)
{
    blackEdgeIcon_onclick(element);
    donePressed();
}

function pegboardIcon_onDblClick(element)
{
    pegboardIcon_onclick(element);
    donePressed();
}

function vintageIcon_onDblClick(element)
{
    vintageIcon_onclick(element);
    donePressed();
}

function setSelectedThemeOnBackside(themeOption, themeName)
{
    resetThemeSelectionOnBackside();
    themeOption.style.backgroundImage = selector.style.backgroundImage;
    document.getElementById("themeLabel").innerText = getLocalizedString('Theme:') + " " + themeName;
}

function switchTheme(themeID) 
{
    setSelectedThemeOnBackside(themeOptionForThemeID(themeID), themeNameForThemeID(themeID));
    webClip.switchToThemeAtIndex_(themeID);
}

function glassIcon_onclick(element)
{
    switchTheme(WebClipTheme.WebClipGlass);
}

function blackEdgeIcon_onclick(element)
{
    switchTheme(WebClipTheme.WebClipBlackEdge);
}

function pegboardIcon_onclick(element)
{
    switchTheme(WebClipTheme.WebClipPegboard);
}

function deckledIcon_onclick(element)
{
    switchTheme(WebClipTheme.WebClipDeckled);
}

function vintageIcon_onclick(element)
{
    switchTheme(WebClipTheme.WebClipVintageCorners);
}

function tornEdgeIcon_onclick(element)
{
    switchTheme(WebClipTheme.WebClipTornEdge);
}

function transitioncomplete() 
{
    resetWidgetCloseBox();
    webClip.didFlipWidget(back.style.display == "none");
    webClip.notifyTransitionIsComplete();
}

function resetThemeSelectionOnBackside()
{    
    document.getElementById("blackEdgeOption").style.backgroundImage = null;
    document.getElementById("deckledOption").style.backgroundImage = null;
    document.getElementById("glassOption").style.backgroundImage = null;
    document.getElementById("tornEdgeOption").style.backgroundImage = null;
    document.getElementById("pegboardOption").style.backgroundImage = null;
    document.getElementById("vintageOption").style.backgroundImage = null;
}

function resetWidgetCloseBox() 
{
    widget.setCloseBoxOffset(3, 3);
}

function savePreferences()
{
    widget.setPreferenceForKey(webClip.clipRectString(), createkey("ClipRect"));
    widget.setPreferenceForKey(webClip.defaultTextEncodingName(), createkey("DefaultTextEncoding"));
    widget.setPreferenceForKey(webClip.fixedWidthFont(), createkey("FixedWidthFont"));
    widget.setPreferenceForKey(webClip.fixedWidthFontSize(), createkey("FixedWidthFontSize"));
    widget.setPreferenceForKey(webClip.minimumFontSize(), createkey("MinimumFontSize"));
    widget.setPreferenceForKey(webClip.pageSizeString(), createkey("PageSize"));
    widget.setPreferenceForKey(webClip.standardFont(), createkey("StandardFont"));
    widget.setPreferenceForKey(webClip.standardFontSize(), createkey("StandardFontSize"));
    widget.setPreferenceForKey(webClip.textSizeMultiplier(), createkey("TextSizeMultiplier"));
    widget.setPreferenceForKey(webClip.themeID(), createkey("Theme"));
    widget.setPreferenceForKey(webClip.URLString(), createkey("URL"));
    widget.setPreferenceForKey(webClip.playAudioOutOfDashboard(), createkey("PlayAudioOutOfDashboard"));
    // The follow parameters may be null.
    var customTextEncodingName = webClip.customTextEncodingName();
    var cookies = webClip.cookiesAsString();
    var signature = webClip.signatureAsString();
    var stylesheet = webClip.userStyleSheetPath();

    if (customTextEncodingName)
      widget.setPreferenceForKey(customTextEncodingName, createkey("CustomTextEncoding"));
    if (cookies)
      widget.setPreferenceForKey(cookies, createkey("CookieProperties"));
    if (signature)
      widget.setPreferenceForKey(signature, createkey("ClipSignature"));
    if (stylesheet)
      widget.setPreferenceForKey(stylesheet, createkey("UserStyleSheetPath"));
}

function onremove()
{
    // Remove instance preferences when removed from Dashboard.
    widget.setPreferenceForKey(null, createkey("ClipRect"));
    widget.setPreferenceForKey(null, createkey("ClipSignature"));   
    widget.setPreferenceForKey(null, createkey("CookieProperties"));
    widget.setPreferenceForKey(null, createkey("CustomTextEncoding"));
    widget.setPreferenceForKey(null, createkey("DefaultTextEncoding"));
    widget.setPreferenceForKey(null, createkey("FixedWidthFont"));
    widget.setPreferenceForKey(null, createkey("FixedWidthFontSize"));
    widget.setPreferenceForKey(null, createkey("MinimumFontSize"));
    widget.setPreferenceForKey(null, createkey("PageSize"));
    widget.setPreferenceForKey(null, createkey("PlayAudioOutOfDashboard"));
    widget.setPreferenceForKey(null, createkey("StandardFont"));
    widget.setPreferenceForKey(null, createkey("StandardFontSize"));
    widget.setPreferenceForKey(null, createkey("TextSizeMultiplier"));
    widget.setPreferenceForKey(null, createkey("Theme"));
    widget.setPreferenceForKey(null, createkey("URL"));
    widget.setPreferenceForKey(null, createkey("UserStyleSheetPath"));
}

function preferenceForKey(key) 
{
    return widget.preferenceForKey(createkey(key));
}

function themeNameForThemeID(themeID)
{
    var themeName;
    
    switch(themeID) {
        case WebClipTheme.WebClipBlackEdge:
            themeName = getLocalizedString("Black Edge");
            break;
        case WebClipTheme.WebClipPegboard:
            themeName = getLocalizedString("Pegboard");
            break;
        case WebClipTheme.WebClipDeckled:
            themeName = getLocalizedString("Deckled Edge");
            break;
        case WebClipTheme.WebClipGlass:
            themeName = getLocalizedString("Glass");
            break;
        case WebClipTheme.WebClipTornEdge:
            themeName = getLocalizedString("Torn Edge");
            break;
        case WebClipTheme.WebClipVintageCorners:
            themeName = getLocalizedString("Vintage Corners");
            break;            
        default:
            themeName = getLocalizedString("Glass");
    }
    
    return themeName;
}

function themeOptionForThemeID(themeID)
{
    var themeOption;
    
    switch(themeID) {
        case WebClipTheme.WebClipBlackEdge:
            themeOption = document.getElementById("blackEdgeOption");
            break;
        case WebClipTheme.WebClipPegboard:
            themeOption = document.getElementById("pegboardOption");
            break;
        case WebClipTheme.WebClipDeckled:
            themeOption = document.getElementById("deckledOption");
            break;
        case WebClipTheme.WebClipGlass:
            themeOption = document.getElementById("glassOption");
            break;
        case WebClipTheme.WebClipTornEdge:
            themeOption = document.getElementById("tornEdgeOption");
            break;
        case WebClipTheme.WebClipVintageCorners:
            themeOption = document.getElementById("vintageOption");
            break;
        default:
            themeOption = document.getElementById("glassOption");
    }
    
    return themeOption;
}

function body_onload()
{
    resetWidgetCloseBox();
    var selectedThemeID = webClip.themeID();
    var selector = document.getElementById("selector");
    selector.setAttribute("style", "background-image: url(Images/selection.png)");
    setSelectedThemeOnBackside(themeOptionForThemeID(selectedThemeID), themeNameForThemeID(selectedThemeID));
    new AppleGlassButton(document.getElementById('done'), getLocalizedString('Done'), donePressed);
    new AppleGlassButton(document.getElementById('editPosition'), getLocalizedString('Edit'), panAndCropPressed);
    document.getElementById("audioCheckbox").checked = !webClip.playAudioOutOfDashboard();
    document.getElementById("audioLabel").innerText = getLocalizedString("Only play audio in Dashboard");
    // Need to coalesce loading and display of the thumbnails with the flip  
    document.getElementById("blackEdgeThumbnail").onload = thumbnailDidLoad;
    document.getElementById("pegboardThumbnail").onload = thumbnailDidLoad;
    document.getElementById("deckledThumbnail").onload = thumbnailDidLoad;
    document.getElementById("glassThumbnail").onload = thumbnailDidLoad;
    document.getElementById("tornEdgeThumbnail").onload = thumbnailDidLoad;
    document.getElementById("vintageThumbnail").onload = thumbnailDidLoad;
}

if (window.widget)
{
    widget.onhide = onhide;
    widget.onshow = onshow;
    widget.ontransitioncomplete = transitioncomplete;
    widget.onremove = onremove;
}

function limit_3 (a, b, c)
{
    return a < b ? b : (a > c ? c : a);
}

function computeNextFloat (from, to, ease)
{
    return from + (to - from) * ease;
}

function animate ()
{
    var T;
    var ease;
    var time  = (new Date).getTime();
    T = limit_3(time-fadeAnimation.startTime, 0, fadeAnimation.duration);
    
    if (T >= fadeAnimation.duration) {
        clearInterval (fadeAnimation.timer);
        fadeAnimation.timer = null;
        fadeAnimation.fadeNow = fadeAnimation.fadeTo;
    } else {
        ease = 0.5 - (0.5 * Math.cos(Math.PI * T / fadeAnimation.duration));
        fadeAnimation.fadeNow = computeNextFloat (fadeAnimation.fadeFrom, fadeAnimation.fadeTo, ease);
    }
    
    webClip.fadeButtonWithOpacity(fadeAnimation.fadeNow);
}

function iFadeIn(e)
{
    // fade in the flip widget
    if (fadeAnimation.timer != null) {
        clearInterval (fadeAnimation.timer);
        fadeAnimation.timer  = null;
    }
    
    var starttime = (new Date).getTime() - 13; // set it back one frame
    fadeAnimation.duration = 500;
    fadeAnimation.startTime = starttime;
    fadeAnimation.element = document.getElementById ('flip');
    fadeAnimation.timer = setInterval ("animate();", 13);
    fadeAnimation.fadeFrom = fadeAnimation.fadeNow;
    fadeAnimation.fadeTo = 1.0;
    animate();
    flipShown = true;
}

function iFadeOut(e)
{
    // fade out the flip widget
    if (fadeAnimation.timer != null) {
        clearInterval (fadeAnimation.timer);
        fadeAnimation.timer  = null;
    }
    
    var starttime = (new Date).getTime() - 13; // set it back one frame
    
    fadeAnimation.duration = 500;
    fadeAnimation.startTime = starttime;
    fadeAnimation.element = document.getElementById ('flip');
    fadeAnimation.timer = setInterval ("animate();", 13);
    fadeAnimation.fadeFrom = fadeAnimation.fadeNow;
    fadeAnimation.fadeTo = 0.0;
    animate();
    flipShown = false;
    
}

function onshow()
{
    webClip.didShowWidget();
}

function onhide()
{
    webClip.didHideWidget();
}

function exitPanAndCrop()
{
    savePreferences();
}

function createkey(key)
{
    return widget.identifier + "-" + key;
}

function enterflip(event)
{
    document.getElementById('fliprollie').style.display = 'block';
    document.getElementById('flip').style.display = 'block';
}

function exitflip(event)
{
    document.getElementById('fliprollie').style.display = 'none';
}

function resizeAndMoveTo(x, y, width, height)
{
    widget.resizeAndMoveTo(x, y, width, height);
}

function openURL(url)
{
    widget.openURL(url);   
}

function thumbnailDidLoad(thumbnail)
{
    if (!isShowingFront())
        return;
    
    thumbnailLoadCount++;
    if (thumbnailLoadCount == numberOfThemes) 
        flip(false);
}

// FIXME: torn edge icon should be done with <canvas>, not the plugin.
function setThumbnailAndFlipToBack(thumbnail, tornEdgeThumbnail) 
{
    var imageSource = "data:image/tiff;base64," + thumbnail;
    var blackEdge = document.getElementById("blackEdgeThumbnail");
    var deckled = document.getElementById("deckledThumbnail");
    var glass = document.getElementById("glassThumbnail");
    var tornEdge = document.getElementById("tornEdgeThumbnail");
    var pegboard = document.getElementById("pegboardThumbnail");
    var vintage = document.getElementById("vintageThumbnail");
    
    // To ensure onload events for thumbnails, nil-out the images.
    blackEdge.src = "";
    deckled.src = "";
    glass.src = "";
    pegboard.src = "";
    tornEdge.src = "";
    vintage.src = "";
    thumbnailLoadCount = 0;

    blackEdge.src = imageSource;
    deckled.src = imageSource;
    glass.src = imageSource;
    pegboard.src = imageSource;
    tornEdge.src = "data:image/tiff;base64," + tornEdgeThumbnail;
    vintage.src = imageSource;
}

function isShowingFront()
{
    return back.style.display = "none";
}

function NSWindowTrackDirtyRegionsHack()
{
    var url = "url(Images/backside.png?" + Math.random() + ")";
    document.getElementById('back').style.backgroundImage = url;    
    setTimeout('widget.performTransition();', 50);
}

function flip(toFront)
{
    var clipper = document.getElementById("clipper");
    var windowWidth  = Math.max(clipper.width, backsideWidth);
    var windowHeight = Math.max(clipper.height, backsideHeight);
    var frontXOffset = Math.round((windowWidth  - clipper.width)  / 2);
    var frontYOffset = Math.round((windowHeight - clipper.height) / 2);
    var backXOffset  = Math.round((windowWidth  - backsideWidth)  / 2);
    var backYOffset  = Math.round((windowHeight - backsideHeight) / 2);

    webClip.setTransitionInProgress();
    if (toFront) {
        back.style.left = backXOffset;
        back.style.top = backYOffset;
        widget.resizeAndMoveTo(window.screenX - backXOffset, window.screenY - backYOffset, windowWidth, windowHeight);
        widget.prepareForTransition("ToFront");
        back.style.display = "none";
        front.style.visibility = "visible";
        front.style.left = 0;
        front.style.top = 0;
        widget.resizeAndMoveTo(window.screenX + frontXOffset, window.screenY + frontYOffset, clipper.width, clipper.height);
    } else {
        front.style.left = frontXOffset;
        front.style.top = frontYOffset;
        widget.resizeAndMoveTo(window.screenX - frontXOffset, window.screenY - frontYOffset, windowWidth, windowHeight);
        widget.prepareForTransition("ToBack");
        front.style.visibility = "hidden";
        back.style.display = "block";
        back.style.left = 0;
        back.style.top = 0;
        widget.resizeAndMoveTo(window.screenX + backXOffset, window.screenY + backYOffset, backsideWidth, backsideHeight);
    }
    setTimeout('NSWindowTrackDirtyRegionsHack();', 0);
}

function flipToFront(event)
{
    flip(true);
}

function donePressed()
{
    savePreferences();
    flipToFront();
}

function panAndCropPressed() 
{
    webClip.editCameraPosition();
    flipToFront();
}

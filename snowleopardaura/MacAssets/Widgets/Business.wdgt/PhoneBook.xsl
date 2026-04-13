<?xml version="1.0" encoding="UTF-8"?>
<!--
Copyright (c) 2005, Apple Computer, Inc.  All rights reserved.
NOTE: Use of this source code is subject to the terms of the Software
License Agreement for Mac OS X, which accompanies the code.  Your use
of this source code signifies your agreement to such license terms and
conditions.  Except as expressly granted in the Software License Agreement
for Mac OS X, no other copyright, patent, or other intellectual property
license or right is granted, either expressly or by implication, by Apple.
-->
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">

<xsl:output method="html" encoding="UTF-8" indent="yes"/>

<xsl:template match="/AppleResults">
	<ul id="resultsList">
		<xsl:apply-templates select="items/anyType"/>
	</ul>
</xsl:template>

<xsl:template match="anyType">
	<xsl:variable name="bizId" select="string(encrypted_id)"/>
	<xsl:variable name="bizName" select="businessname"/>
	<xsl:variable name="firstBizLink">
		<xsl:choose>
			<xsl:when test="position()=1">firstBizLink</xsl:when>
			<xsl:otherwise></xsl:otherwise>
		</xsl:choose>
	</xsl:variable>
	<li>
		<div id="name-{$bizId}" class="name">
			<a id="{$firstBizLink}" href="#" onclick="bizNameClicked(event, '{$bizId}');">
				<xsl:value-of select="$bizName"/>
			</a>		
		</div>
		<div class="add">
			<button class="addButton" onclick="addButtonClicked(event, '{$bizId}');">
			<xsl:text>Add "</xsl:text><xsl:value-of select="$bizName"/><xsl:text>" to address book</xsl:text>
		</button>
		</div>
		<div id="phone-{$bizId}" class="phone">
			<a href="#" onclick="phoneClicked(event);">
				<xsl:variable name="rawPhone" select="string(phone)"/>
				<xsl:variable name="phone">
					<xsl:value-of select="
						concat(substring-before($rawPhone, ')'), 
								') ',
								substring-after($rawPhone, ')'))
						"/>
				</xsl:variable>
				<xsl:value-of select="$phone"/>
			</a>
		</div>
		<div class="address">
			<a href="#" onclick="addressClicked(event, '{$bizId}');">
				<span id="street-{$bizId}" class="street">
					<xsl:value-of select="address"/>
				</span>
				<span id="city-{$bizId}" class="city">
					<xsl:value-of select="city"/>
				</span>
				<xsl:text>,</xsl:text>
				<span id="state-{$bizId}" class="state">
					<xsl:value-of select="state"/>
				</span>
				<xsl:text> </xsl:text>
				<span id="postalCode-{$bizId}" class="postalCode">
					<xsl:value-of select="zipcode"/>
				</span>
			</a>
		</div>
	</li>
</xsl:template>

</xsl:stylesheet>
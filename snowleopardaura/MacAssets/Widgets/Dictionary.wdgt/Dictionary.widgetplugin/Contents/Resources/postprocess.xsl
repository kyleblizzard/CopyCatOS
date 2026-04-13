<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet version="1.0"
				xmlns:xsl="http://www.w3.org/1999/XSL/Transform" 
				xmlns:o="http://oup.dataformat.com/doc/OUP_DTD_Dictionary.html"
				xmlns="http://www.w3.org/1999/xhtml">
<xsl:output method="xml" encoding="UTF-8" indent="no" omit-xml-declaration="yes" />

<xsl:param name="pr" select="'US'"/> <!-- US, US_IPA or UK_IPA or empty to display all -->


<xsl:template match="/">
		<xsl:apply-templates/>
</xsl:template>

<!-- default rule for oxford elements -->
<xsl:template match="o:*">
	<span>
		<xsl:attribute name="class"><xsl:value-of select="local-name()"/></xsl:attribute>
		<xsl:apply-templates select="@*|node()" />
	</span>
</xsl:template>

<!-- expand o:pr only when the type maches the preference -->
<xsl:template match="o:pr">
	<xsl:if test="$pr=@type or substring-after($pr,'_')=@type or $pr=''">
		<span>
			<xsl:attribute name="class"><xsl:value-of select="local-name()"/></xsl:attribute>
			<xsl:apply-templates select="@*|node()" />
		</span>
	</xsl:if>
</xsl:template>

<!-- default rule for html elements (actually this matches with any elements. Oxford elements, however, match with the more specific one due to the matchng priority) -->
<xsl:template match="*">
	<xsl:copy>
         <xsl:apply-templates select="@*|node()"/>
	</xsl:copy>
</xsl:template>

<!-- attributes and text nodes are copied as is -->
<xsl:template match="@*|text()">
	<xsl:copy/>
</xsl:template>


</xsl:stylesheet>

<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
				xmlns:o="http://oup.dataformat.com/doc/OUP_DTD_Dictionary.html"
				version="1.0">
<xsl:output method="xml" encoding="UTF-8" indent="no"/>

<!--
================================================================
- This XSLT is to remove some invalid nodes.
  e.g. 
  + If all <o:sense> blocks in an entry are censored out, 
    the senseBlock (<o:SB>) does not contain any <o:sense> block.
    In such case, whole the <o:SB> should be removed.  
  + If all senseBlock (<o:SB>) are removed, the entry (<o:ent>) 
    should be removed to hide other blocks such as for phases or 
    usage for that removed meaning.
  + If all entry (<o:ent>) blocks are removed, the dictionary (<dic>)
    should be removed.

i.e.:
- If a senseBlock (<o:SB>) doesn't contain <o:sense> block, 
  it is not a valid senseBlock.  It is not copied to the destination.
- If an entry (o:ent) doesn't contain a valid senseBlock (<o:SB>), 
  it is not a valid entry.  It is not copied to the destination.
- If an dic doesn't contain a valid entry (<o:ent>), 
  it is not a valid dic.  It is not copied to the destination.
================================================================
-->

<!--
================================================================
Template for "/"
================================================================
-->
<xsl:template match="/">
	<xsl:apply-templates />
</xsl:template>


<!--
================================================================
Template for "dic-list"

Process a dic-list.
================================================================
-->
<xsl:template match="dic-list">
	<xsl:copy>
	<xsl:copy-of select="./@*" />
		<xsl:apply-templates />
	</xsl:copy>
</xsl:template>


<!--
================================================================
Template for "dic"

Process a dic if it contains <o:sense> block in <o:SB> block in <o:ent> block.
================================================================
-->
<xsl:template match="dic">
	<xsl:if test="count(./o:ent/o:SB//o:sense) > 0">
		<xsl:copy>
		<xsl:copy-of select="./@*" />
			<xsl:apply-templates />
		</xsl:copy>
	</xsl:if>
</xsl:template>


<!--
================================================================
Template for "o:ent"

Process an entry if it contains <o:sense> block in <o:SB> block.
================================================================
-->
<xsl:template match="o:ent">
	<xsl:if test="count(./o:SB//o:sense) > 0">
		<xsl:copy>
		<xsl:copy-of select="./@*" />
			<xsl:apply-templates />
		</xsl:copy>
	</xsl:if>
</xsl:template>


<!--
================================================================
Template for "o:SB"

Process a senseBlock if it contains <o:sense> block.
================================================================
-->
<xsl:template match="o:SB">
	<xsl:if test="count(.//o:sense) > 0">
		<xsl:copy>
		<xsl:copy-of select="./@*" />
			<xsl:apply-templates />
		</xsl:copy>
	</xsl:if>
</xsl:template>


<!--
================================================================
Copy node as is.
================================================================
-->
<xsl:template match="@*|node()">
	<xsl:copy-of select="." />
</xsl:template>


</xsl:stylesheet>

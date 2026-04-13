<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
				xmlns:o="http://oup.dataformat.com/doc/OUP_DTD_Dictionary.html"
				version="1.0">
<xsl:output method="xml" encoding="UTF-8" indent="no"/>


<!--
================================================================
- This XSLT is to remove some kind of blocks that contains 
  any of the following node.
  + <o:regLabel> node that contains one of the following.
    - 'vulgar slang'
    - 'derogatory'
    - 'offensive'

e.g.
- If a <o:sense> block contains such regLabel, the <o:sense> block 
  is remove.  i.e. It is not copied to the destination.
- If <o:prelim> of <o:Prelim> of a senseBlock (<o:SB>) contains 
  such regLabel, whole the <o:SB> is removed.
- etc.
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
Copy the node, and copy the attributes of the node.
And, call a template for each child node.
================================================================
-->
<xsl:template match="dic-list">
	<xsl:copy>
	<xsl:copy-of select="./@*" />
	<xsl:for-each select="./dic">
		<xsl:call-template name="dic" />
	</xsl:for-each>
	</xsl:copy>
</xsl:template>


<!--
================================================================
dic

Process a dic.
================================================================
-->
<xsl:template name="dic">
	<xsl:copy>
	<xsl:copy-of select="./@*" />
	<xsl:for-each select="./o:ent">
		<xsl:call-template name="ent" />
	</xsl:for-each>
	</xsl:copy>
</xsl:template>


<!--
================================================================
ent

Process an entry.
Check if the hwGrp contains a bad regLabel or not.
If found, do nothing.
If not found, call template for each direct child to copy if appropriate.
================================================================
-->
<xsl:template name="ent">
	<xsl:copy>
	<xsl:copy-of select="./@*" />
	<!-- Check all the o:regLabel nodes in the o:hwGrp node -->
	<xsl:variable name="toHide">
		<xsl:call-template name="check-sub-tree">
			<xsl:with-param name="node" select="./o:hwGrp" />
		</xsl:call-template>
	</xsl:variable>
	<!-- test begin : put the check results -->
		<!--
		<font color="blue"> [+ <xsl:value-of select="$toHide" /> +] </font>
		-->
	<!-- test end -->
	<!-- Put text according to the check results -->
	<xsl:choose>
		<xsl:when test="contains($toHide,'1') or contains($toHide,'2') or contains($toHide,'3')">
			<!-- test begin : output the hidden entry in color -->
				<xsl:call-template name="copy-sub-tree-in-color">
					<xsl:with-param name="node" select="." />
				</xsl:call-template>
			<!-- test end -->
		</xsl:when>
		<xsl:otherwise>
			<xsl:apply-templates />
		<!--
			<xsl:for-each select="./node()">
				<xsl:call-template name="part" />
			</xsl:for-each>
		-->
		</xsl:otherwise>
	</xsl:choose>
	</xsl:copy>
</xsl:template>


<!--
================================================================
Template for "o:SB"

Process a senseBlock.
================================================================
-->
<xsl:template match="o:SB">
<!--
	<font color="brown">
-->
	<xsl:copy>
	<xsl:copy-of select="./@*" />
	<!-- Check all the o:regLabel nodes in the o:prelim or o:Prelim node -->
	<xsl:variable name="toHide">
		<xsl:call-template name="check-sub-tree">
			<xsl:with-param name="node" select="./o:prelim | ./o:Prelim" />
		</xsl:call-template>
	</xsl:variable>
	<!-- test begin : put the check results -->
		<!--
		<font color="darkblue"> [+ <xsl:value-of select="$toHide" /> +] </font>
		-->
	<!-- test end -->
	<!-- Put text according to the check results -->
	<xsl:choose>
		<xsl:when test="contains($toHide,'1') or contains($toHide,'2') or contains($toHide,'3')">
			<!-- Hide the whole senseBlock -->
			<!-- test begin : output the hidden senseBlock in color -->
				<xsl:call-template name="copy-sub-tree-in-color">
					<xsl:with-param name="node" select="." />
				</xsl:call-template>
			<!-- test end -->
		</xsl:when>
		<xsl:otherwise>
			<!-- Process each child in the senseBlock -->
			<xsl:apply-templates />
		<!--
			<xsl:for-each select="./node()">
				<xsl:call-template name="part" />
			</xsl:for-each>
		-->
		</xsl:otherwise>
	</xsl:choose>
	</xsl:copy>
<!--
	</font>
-->
</xsl:template>


<!--
================================================================
Template for "o:sense|o:sUse"

If ( it contains labels to hide as a *direct* child )
	{ Do nothing. (i.e. Hide the whole node.) }
Else
	{ Shallow copy the node, and process the children. }

Treated the node as a *composite*.  Process each of the children individually.

Note that this template *does not* check the indirect children.
================================================================
-->
<xsl:template match="o:sense | o:sense/o:sUse | o:sUse">
<!--
	<font color="tomato">
-->
	<!-- Check all the o:regLabel nodes as the direct child -->
	<xsl:variable name="toHide">
		<xsl:call-template name="check-direct-children">
			<xsl:with-param name="node" select="." />
		</xsl:call-template>
	</xsl:variable>
	<!-- Put text according to the check results -->
	<xsl:choose>
		<xsl:when test="contains($toHide,'1') or contains($toHide,'2') or contains($toHide,'3')">
			<!-- Hide the whole sense -->
			<!-- test begin : output the hidden node in color -->
				<xsl:call-template name="copy-sub-tree-in-color">
					<xsl:with-param name="node" select="." />
				</xsl:call-template>
			<!-- test end -->
		</xsl:when>
		<xsl:otherwise>
			<xsl:copy>
			<xsl:copy-of select="./@*" />
				<!-- Process each child in the sense|sUse -->
				<xsl:apply-templates />
			</xsl:copy>
		</xsl:otherwise>
	</xsl:choose>
<!--
	</font>
-->
</xsl:template>



<!--
================================================================
Template for "o:MS|o:meta|o:hwGrp|o:encBlock|o:etymBlock|o:specialFeature | o:sense/node() "

Process nodes that are treated as a *unit*.
Copy or hide whole the node.

If ( it contains labels to hide as a *direct* or *indirect* child )
	{ Do nothing. (i.e. Hide the whole node.) }
Else
	{ Deep copy the node. }

	//{ Shallow copy the node, and process the children. }
================================================================
-->
<xsl:template match="o:MS|o:meta|o:hwGrp|o:encBlock|o:etymBlock|o:specialFeature | o:sense/node() ">
<!--
	<font color="salmon">
-->
	<!-- Check all the o:regLabel nodes as the *direct* or *indirect* child -->
	<xsl:variable name="toHide">
		<xsl:call-template name="check-sub-tree">
			<xsl:with-param name="node" select="." />
		</xsl:call-template>
	</xsl:variable>
	<!-- Put text according to the check results -->
	<xsl:choose>
		<xsl:when test="contains($toHide,'1') or contains($toHide,'2') or contains($toHide,'3')">
			<!-- Hide the whole sense -->
			<!-- test begin : output the hidden node in color -->
				<xsl:call-template name="copy-sub-tree-in-color">
					<xsl:with-param name="node" select="." />
				</xsl:call-template>
			<!-- test end -->
		</xsl:when>
		<xsl:otherwise>
			<xsl:copy-of select="." />
		</xsl:otherwise>
	</xsl:choose>
<!--
	</font>
-->
</xsl:template>



<!-- 
================================================================
Template for "o:prelim | o:Prelim | o:phrBlock|o:pvBlock|o:drvBlock|o:usage|o:wordlist"

Process nodes that are treated as a *composite*.
Copy the node, and apply "part" template for each child.
================================================================
-->
<xsl:template match="o:prelim | o:Prelim | o:phrBlock|o:pvBlock|o:drvBlock|o:usage|o:wordlist">
<!-- 
	<font color="brown">
-->
	<xsl:copy>
	<xsl:copy-of select="./@*" />
	<xsl:for-each select="./node()">
		<xsl:call-template name="part" />
	</xsl:for-each>
	</xsl:copy>
<!-- 
	</font>
-->
</xsl:template>


<!--
================================================================
Template for "@*|node()"

Just copy the node.
================================================================
-->
<xsl:template match="@*|node()">
<!-- test begin : @@@@ This should be commented out @@@@ -->
<!--
	<font color="red">
-->
<!-- test end -->
	<xsl:copy-of select="." />
<!-- test begin : @@@@ This should be commented out @@@@ -->
<!--
	</font>
-->
<!-- test end -->
</xsl:template>



<!--
================================================================
part

Process a part.

If ( it contains labels to hide as a *direct* or *indirect* child )
	{ Do nothing. (i.e. Hide the whole node.) }
Else
	{ Deep copy whole the node. }

If the node contains a bad regLabel, do nothing.
If not, just copy whole the node.
================================================================
-->
<xsl:template name="part">
	<!-- Check all the o:regLabel nodes in the entry -->
	<xsl:variable name="toHide">
		<xsl:call-template name="check-sub-tree">
			<xsl:with-param name="node" select="." />
		</xsl:call-template>
	</xsl:variable>
	<!-- test begin : put the check results -->
		<!--
		<font color="blue"> [- <xsl:value-of select="$toHide" /> -] </font>
		-->
	<!-- test end -->
	<!-- Put text according to the check results -->
	<xsl:choose>
		<xsl:when test="contains($toHide,'1') or contains($toHide,'2') or contains($toHide,'3')">
			<!-- test begin : output the hidden part in color -->
				<xsl:call-template name="copy-sub-tree-in-color">
					<xsl:with-param name="node" select="." />
				</xsl:call-template>
			<!-- test end -->
		</xsl:when>
		<xsl:otherwise>
			<xsl:copy-of select="." />
		</xsl:otherwise>
	</xsl:choose>
</xsl:template>


<!--
================================================================
check-sub-tree

Sub-routine to check a sub-tree.
Returns a sequence of 0, 1, 2, 3.
================================================================
-->
<xsl:template name="check-sub-tree">
	<xsl:param name="node" />
	<xsl:variable name="toHide">
		<xsl:for-each select="$node//o:regLabel">			<!-- sub-tree -->
			<xsl:choose>
				<xsl:when test="contains(.,'vulgar slang')">1</xsl:when>
				<xsl:when test="contains(.,'derogatory')">2</xsl:when>
				<xsl:when test="contains(.,'offensive')">3</xsl:when>
				<xsl:otherwise>0</xsl:otherwise>
			</xsl:choose>
		</xsl:for-each>
	</xsl:variable>
	<xsl:value-of select="$toHide" />
</xsl:template>


<!--
================================================================
check-direct-children

Sub-routine to check the direct children.
Returns a sequence of 0, 1, 2, 3.
================================================================
-->
<xsl:template name="check-direct-children">
	<xsl:param name="node" />
	<xsl:variable name="toHide">
		<xsl:for-each select="$node/o:regLabel">		<!-- direct children -->
			<xsl:choose>
				<xsl:when test="contains(.,'vulgar slang')">1</xsl:when>
				<xsl:when test="contains(.,'derogatory')">2</xsl:when>
				<xsl:when test="contains(.,'offensive')">3</xsl:when>
				<xsl:otherwise>0</xsl:otherwise>
			</xsl:choose>
		</xsl:for-each>
	</xsl:variable>
	<xsl:value-of select="$toHide" />
</xsl:template>


<!--
================================================================
copy-sub-tree-in-color

Sub-routine for debugging.
Comment out the body for deployment.
Turn the body alive when debugging.
Then, it copies the sub-tree in a <censored> block with color, instead of removing it.
================================================================
-->
<xsl:template name="copy-sub-tree-in-color">
	<xsl:param name="node" />
<!-- test begin : @@@@ This should be commented out @@@@ -->
<!--
	<censored>
		<font color="teal">
			<xsl:copy-of select="$node" />
		</font>
	</censored>
-->
<!-- test end -->
</xsl:template>


</xsl:stylesheet>

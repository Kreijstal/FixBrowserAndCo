<div style="width: 800px; margin: 25px auto">
<a href="https://web.archive.org/" style="display: block; float: left; margin-right: 10px">
<img src="https://web.archive.org/static/bower_components/wayback-search-js/dist/187b83bcdd106961d05e5587cce04f4d.png" width="183" height="65" border="0">
<!--
https://web.archive.org/static/bower_components/wayback-search-js/dist/187b83bcdd106961d05e5587cce04f4d.png
https://archive.org/images/WaybackLogoSmall.png
https://web.archive.org/static/images/logo_archive-sm.png
-->
</a>

Explore more than 916 billion <a href="https://blog.archive.org/2016/10/23/defining-web-pages-web-sites-and-web-captures/">web pages</a>
saved over time

<form>
<input type="text" name="url" placeholder="Enter a URL or words related to a site's home page" value="${url}" style="width: 585px; border: 1px solid #CCC; border-radius: 5px; padding: 5px 2px 5px 10px; font-size: 17px; font-weight: 300; margin-top: 10px">
</form>
</div>

<div style="margin: 25px auto" class="web_message">
Find the Wayback Machine useful?
<a class="beta_button" href="https://archive.org/donate/">DONATE</a>
</div>

<div style="text-align: center">
<?if defined(years)?>
	<?begin years?>
		<a href="${link}" target="timeline">${year}</a>
	<?end?>
<?endif?>
</div>

<div style="text-align: center">
<?if defined(latest_link)?>
<iframe name="timeline" src="${latest_link}" style="width: 100%; max-width: 1000px; height: 400px; border: 2px solid #ccc; padding: 2px; margin-top: 10px"></iframe>
<?endif?>
</div>

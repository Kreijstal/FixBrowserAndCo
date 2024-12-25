<html>
<head>
	<title>${title}</title>
</head>
<body>

Confirm going to URL:<br>
<b style="font-family: monospace;">${url}</b><br>

<?if defined(links)?>
<br><hr><br>List of URLs contained in parameters:<br>
<?begin links?>
	<?if !first(links)?><br><?endif?>
	<b style="font-family: monospace;">${url}</b><br>
	<a href="${url_link}">Yes</a><br>
<?end?>
<br><hr><br>Or go to original URL:<br>
<?endif?>

<a href="${url_link}">Yes</a>

</body>
</html>

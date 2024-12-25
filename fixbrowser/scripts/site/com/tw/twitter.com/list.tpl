<html>
<head>
	<title>${title}</title>
	<style>
		* { font-family: sans-serif; font-size: 14px; line-height: 1.3; }
		body { background-color: #e8e8e8; color: #000000; }
		div.posts { background-color: #ffffff; max-width: 800px; margin: 0 auto; }
		div.header { background-color: #50a7e6; color: #ffffff; font-weight: bold; padding: 4px 6px; }
		div.content { padding: 4px 6px; padding-bottom: 10px; }
		div.content img { max-width: 100%; margin-top: 10px; }
	</style>
</head>
<body>

<div class="posts">
<?begin posts?>
	<div class="post">
	<div class="header">${full_name} @${username} ${time}</div>
	<div class="content">
		${text}
		<?if defined(image)?>
		<br>
		<a href="${image}"><img src="${image}"></a>
		<?endif?>
	</div>
	</div>
<?end?>
</div>

</body>
</html>

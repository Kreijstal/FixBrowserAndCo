<html>
<head>
	<style>
		body { background: #FFF; color: #2A2E2E; font-family: sans-serif; font-size: 15px; margin: 20px 10px; }
		a { color: #2F7BBF; font-weight: bold; text-decoration: none; }
		div.post { margin: 20px 0; }
		div.avatar { width: 48px; float: left; height: 48px; }
		div.avatar img { border-radius: 3px; }
		div.header { margin: 5px 0; margin-left: 56px; }
		div.content { margin-left: 56px; }
		span.author { font-size: 13px; }
		span.bullet { font-size: 13px; color: #C2C6CC; }
		span.date { font-size: 12px; color: #656C7A; }
	</style>
</head>
<body>

Comments provided by <a href="https://disqus.com/">Disqus</a><br><br>

<div id="posts">
<?begin posts?>
	<div class="post" style="margin-left: ${indent}px">
	<div class="avatar"><img src="${image}" width="48" height="48"></div>
	<div class="header">
		<span class="author"><a href="${profile_url}">${name}</a></span>
		<span class="bullet">&bullet;</span>
		<span class="date">${date}</span>
	</div>
	<div class="content">
	${message}
	</div>
	<div style="clear: both"></div>
	</div>
<?end?>
</div>

</body>
</html>

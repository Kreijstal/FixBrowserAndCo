<html>
<head>
	<title>FixProxy</title>
	<style>
		input[name='fixproxy_url'] {
			border: 1px solid #888;
			border-radius: 5px;
			padding: 5px 8px;
			font-size: 18px;
			width: 60%;
		}
		input[type='submit'] {
			border: 1px solid #888;
			border-radius: 5px;
			background: #ccc;
			font-size: 16px;
			padding: 5px 8px;
		}
		input[type='submit']:hover {
			border: 1px solid #444;
			background: #444;
			color: #fff;
			cursor: pointer;
		}
		input[name^='search_'] {
			padding-left: 14px;
			padding-right: 14px;
		}
	</style>
</head>
<body>

<div style="text-align: center; margin-top: 15%">
<form action="/" method="get">
<input type="text" name="fixproxy_url" placeholder="Enter URL or search terms" autofocus style="width: 60%">
<input type="submit" value="Go"><br><br>
<input type="submit" name="search_duckduckgo" value="Search on DuckDuckGo"> &nbsp;
<input type="submit" name="search_wikipedia" value="Search on Wikipedia"> &nbsp;
<input type="submit" name="search_archive" value="Search on Archive"><br>
</form>
</div>

</body>
</html>

<html>
<head>
	<title>YouTube</title>
	<style>
		#logo {
			position: relative;
			width: 180px;
			height: 50px;
			overflow: hidden;
			display: block;
			margin: 0;
			padding: 0;
		}
		
		#logo > span {
			display: block;
			position: absolute;
			background: url(https://lh3.googleusercontent.com/cOKUOqMLCbZu6y4_J0HvSFylHiFwZVpTqN0_wYOeT3Dms5825uIc_Vz1il3rsLUdLaeIgWGiYlkXOpkqROt_GsBpcsvmI2eleTxa);
			width: 351px;
			height: 512px;
			left: -90px;
			top: -50px;
		}

		#logo > span > span {
			display: none;
		}

		div.video {
			display: inline-block;
			width: 320px;
			height: 240px;
			overflow: hidden;
			margin: 5px;
		}

		div.thumbnail {
			width: 320px;
			height: 180px;
			background-size: cover;
		}

		div.title {
			width: 320px;
		}
	</style>
</head>
<body>

<h1 id="logo"><span><span>YouTube</span></span></h1>

<center style="margin-top: 15px">
<?begin videos?><!--
--><div class="video">
<div class="thumbnail" style="background-image: url(${thumbnail})"></div>
<div class="title">
<a href="${url}">${title}</a>
</div>
</div><!--
--><?end?>
</center>

<pre>${data}</pre>

</body>
</html>

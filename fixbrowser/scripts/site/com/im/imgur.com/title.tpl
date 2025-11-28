<html>
<head>
	<title>Imgur: The magic of the Internet</title>
	<style>${style}</style>
</head>
<body>

<a href="/"><h1><span>Imgur</span></h1></a>

<h2>Most viral</h2>

<div class="box-container">
<?begin media?>
<div class="box" style="width: ${width}px">
<?if defined(image_url)?>
<a href="${image_url}"><img src="${image_url}" style="width: ${width}px; height: ${height}px" class="image"></a><br><br>
<?endif?>
<?if defined(video_url)?>
<video controls style="width: ${width}px; height: ${height}px" class="video">
	<source src="${video_url}" type="${mime_type}"/>
	<a href="${video_url}">Link to video</a>
</video>
<?endif?>
<div class="box-description">
<a href="${url}">${title}</a>
</div> <!-- .box-description -->
</div> <!-- .box -->
<?end?>
</div> <!-- .box-container -->

</body>
</html>

<html>
<head>
	<title>${title} - Imgur</title>
	<style>${style}</style>
</head>
<body>

<a href="/"><h1><span>Imgur</span></h1></a>

<h2>${title}</h2>

<p class="metadata">
${views} Views &bull; ${created}
</p>

<div class="media-container">
<?begin media?>
<?if defined(image_url)?>
<a href="${image_url}"><img src="${image_url}" class="image media-${orientation}"></a><br><br>
<?endif?>
<?if defined(video_url)?>
<video controls class="video media-${orientation}">
	<source src="${video_url}" type="${mime_type}"/>
	<a href="${video_url}">Link to video</a>
</video>
<?endif?>
<br><br>
<?end?>
</div> <!-- .media-container -->

</body>
</html>

<html>
<head>
	<title>${page_title}</title>
	<style>
		body { font-family: sans-serif; font-size: 16px; line-height: 1.3; }
		body { background-color: #fff; color: #000000; }
		#page { max-width: 1280px; margin: 0 auto; }
		#main { margin-right: 440px; padding: 20px; }
		#side { width: 420px; float: right; height: 1000px; padding: 20px; box-sizing: border-box; }
		#side #state { font-size: 14px; font-weight: bold; text-transform: uppercase; letter-spacing: 1px; line-height: 25px; border-bottom: 1px solid #DDD; color: #088366; }
		#side h1 { font-size: 24px; font-weight: bold; }
		#side h2 { font-size: 11px; font-weight: bold; color: #6a6a6a; text-transform: uppercase; }
		#side .tagline { color: #6a6a6a; }
		#side #project-owner { font-size: 12px; }
		#side #perks { width: 75%; }
		#side .perk { border: 1px solid #C8C8C8; }
		#side .perk .desc { padding: 20px; }
		#image { width: 695px; max-width: 100%; height: 460px; text-align: center; border: 1px solid #CCC; position: relative; }
		#image > span { position: absolute; bottom: 50%; left: 0; right: 0; }
		#main h2 { font-size: 14px; text-transform: uppercase; line-height: 25px; border-bottom: 1px solid #DDD; color: #6a6a6a; }
	</style>
</head>
<body>

<div id="page">

<div id="side">

<div id="state">Funding</div>

<h1>${title}</h1>

<p class="tagline">
${tagline}
</p>

<h2>Project owner</h2>

<div id="project-owner">
<?if defined(owner_image)?>
<img src="${owner_image}" width="50" height="50" style="float: left; margin-right: 5px;">
<?endif?>
<div><b>${owner_name}</b><br>${owner_location}</div>
<div style="clear: both"></div>
</div> <!-- #project-owner -->

<br>
${raised} ${currency_code} raised<br>
${raised_percentage}% of ${raised_goal} ${funding_type}<br>
<br>
Date start: ${date_start}<br>
Date end: ${date_end}<br>

<br>
<div id="perks">
<?begin perks?>
<div class="perk">
<img src="${image}" style="width: 100%">
<div class="desc">
<b>${price}</b><br>
<b>${label}</b><br>
Estimated delivery:<br>
${delivery}<br>
<b>${number_claimed}</b> out of <b>${number_available}</b> claimed
</div> <!-- .desc -->
</div> <!-- .perk -->
<br><br>
<?end?>
</div>

</div> <!-- #side -->

<div id="main">

<div id="image"><span>main images not implemented yet</span></div>

<h2>Overview</h2>

<div id="overview">
<img src="${overview_image}" style="float: left; max-height: 100px; margin-right: 10px; margin-bottom: 10px;">
${overview}
<div style="clear: left"></div>
</div>

<h2>Description</h2>

<div id="description">
${description}
</div>

</div> <!-- #main -->
</div> <!-- #page -->

</body>
</html>

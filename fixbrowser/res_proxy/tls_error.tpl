<html>
<head>
	<title>Certificate Warning</title>
	<style>
		body {
			font-family: sans-serif;
			font-size: 15px;
			text-align: center;
		}

		table.certificate {
			border-collapse: collapse;
			border: 2px solid #888;
			font-family: sans-serif;
			font-size: 11px;
		}

		table.certificate > tbody > tr > td {
			border: 1px solid #BBB;
			padding: 3px 6px;
		}

		table.certificate table {
			border-collapse: collapse;
			border: 1px solid #FFF;
			font-family: sans-serif;
			font-size: 11px;
		}

		table.certificate table > tbody > tr > td {
			border: 1px solid #BBB;
			padding: 1px 3px;
		}
		
		table.certificate table table > tbody > tr > td {
			border: none;
		}

		div.cert-switch {
			display: inline-block;
			background: #eee;
			text-align: left;
			padding: 5px;
			border: 2px solid #888;
		}

		div.cert-switch h2 {
			font-size: 14px;
			font-weight: bold;
			margin: 0;
			margin-bottom: 5px;
			text-align: center;
		}
		
		div.cert-switch ul, div.cert-switch li {
			display: block;
			padding: 0;
			margin: 0;
		}

		div.cert-switch label {
			display: block;
			cursor: pointer;
			border: 1px solid transparent;
			padding: 2px 4px;
		}

		input#show-detail:not(:checked) ~ div table.certificate tr.detail {
			display: none;
		}

		span.delim {
			display: block;
		}
	</style>
</head>
<body>

<h1>Certificate Warning</h1>

<p>
<b>Reason:</b> ${error}
</p>

<?begin certs?>
<input type="radio" name="cert" value="${id}" id="cert-${id}" style="display: none"<?if first(certs)?> checked<?endif?>>
<style>
input#cert-${id}:not(:checked) ~ div#cert-${id} {
	display: none;
}
input#cert-${id}:checked ~ div label[for='cert-${id}'] {
	background: #fff;
	border: 1px solid #888;
}
</style>
<?end?>

<div class="cert-switch">
<h2>Certificate Hierarchy</h2>
<ul>
<?begin certs?>
	<li><label for="cert-${id}">${name}</label></li>
<?end?>
</ul>
</div>

<br><br>
<input type="checkbox" id="show-detail"> <label for="show-detail">Show details</label>
<br><br>

<?begin certs?>
<div class="cert" id="cert-${id}">
${cert}
</div>
<?end?>

<br>
<form action="/accept-certificate" method="post">
<input type="hidden" name="csrf_token" value="${csrf_token}">
<input type="hidden" name="url" value="${url}">
<input type="hidden" name="hash" value="${hash}">
<!--
<label><input type="checkbox" name="permanent"> Add to permanent trust</label><br><br>
-->
<input type="submit" value="Trust certificate and continue">
</form>

</body>
</html>

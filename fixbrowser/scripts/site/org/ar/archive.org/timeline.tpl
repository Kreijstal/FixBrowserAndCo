<html>
<head>
	<style>
		body {
			background-color: #FFF;
			color: #000;
			font-family: Arial, "Helvetica Neue", sans-serif;
			font-size: 14px;
			line-height: 140%;
		}

		a {
			color: #428BCA;
			text-decoration: none;
		}

		a:hover {
			text-decoration: underline;
		}
	</style>
</head>
<body>

<?if defined(fail)?>
(failed to fetch)<br>
<?endif?>
<?if defined(days)?>
<?begin days?>
${date}
	<?begin times?>
		<?if !first(times)?>|<?endif?>
		<a href="${link}" target="_parent">${time}</a>
	<?end?>
<br>
<?end?>
<?endif?>

</body>
</html>

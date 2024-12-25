function resizeIFrame(iframe, body, autoWidth, autoHeight)
{
	var compStyle = getComputedStyle(body);

	// TODO: horizontal width seems to not work
	// TODO: handle margins in different units

	if (autoWidth) {
		var left = parseInt(compStyle.getPropertyValue('margin-left'));
		var right = parseInt(compStyle.getPropertyValue('margin-right'));
		var width = (body.clientWidth + left + right) + 'px';
		if (iframe.style.width != width) {
			iframe.style.width = width;
		}
	}

	if (autoHeight) {
		var top = parseInt(compStyle.getPropertyValue('margin-top'));
		var bottom = parseInt(compStyle.getPropertyValue('margin-bottom'));
		var height = (body.clientHeight + top + bottom) + 'px';
		if (iframe.style.height != height) {
			iframe.style.height = height;
		}
	}
}

function autoResizeIFrame(iframe, autoWidth, autoHeight) {
	var body = iframe.contentWindow.document.body;
	resizeIFrame(iframe, body, autoWidth, autoHeight);

	if (typeof MutationObserver !== 'undefined') {
		new MutationObserver(function() {
			resizeIFrame(iframe, body, autoWidth, autoHeight);
		}).observe(body, {
			"attributes": true,
			"characterData": true,
			"childList": true,
			"subtree": true
		});
	}
	else {
		setInterval(function() {
			resizeIFrame(iframe, body, autoWidth, autoHeight);
		}, 1000);
	}
}

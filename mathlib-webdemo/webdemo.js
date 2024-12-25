/*
 * This file was written in 2024 by Martin Dvorak <jezek2@advel.cz>
 * You can download latest version at http://public-domain.advel.cz/
 *
 * To the extent possible under law, the author(s) have dedicated all
 * copyright and related and neighboring rights to this file to the
 * public domain worldwide. This software is distributed without any
 * warranty.
 *
 * You should have received a copy of the CC0 Public Domain Dedication
 * along with this software. If not, see:
 * http://creativecommons.org/publicdomain/zero/1.0/ 
 */

var demoElement = null;
var convertorElement = null;
var expressionInput = null;
var restartFunc = null;
var canvas = null;
var ctx = null;

var memory = null;
var mem8 = null;
var mem16 = null;
var mem32 = null;
var memF32 = null;
var memF64 = null;
var exports = null;
var table = null;

function loadWASM(contFunc) {
	var xhr = new XMLHttpRequest();
	xhr.open("GET", "webdemo.wasm");
	xhr.responseType = "arraybuffer";
	xhr.send();

	xhr.onload = function() {
		var imports = {
			"env": {
				"refresh_memory": function () {
					memory = exports.memory;
					mem8 = new Uint8Array(memory.buffer);
					mem16 = new Uint16Array(memory.buffer);
					mem32 = new Int32Array(memory.buffer);
					memF32 = new Float32Array(memory.buffer);
					memF64 = new Float64Array(memory.buffer);
				},
				"print": function (ptr) {
					var s = [], len=0;
					for (;;) {
						var c = mem8[ptr++];
						if (c == 0) break;
						s[len++] = c;
					}
					console.log(String.fromCharCode.apply(null, s));
				},
				"get_monotonic_time": function() {
					return performance.now();
				},
				"timer_start": function (interval, repeat, func, data) {
					if (repeat) {
						return setInterval(function() {
							table.get(func)(data);
						}, interval);
					}
					else {
						return setTimeout(function() {
							table.get(func)(data);
						}, interval);
					}
				},
				"timer_stop": function (id) {
					clearTimeout(id);
				}
			}
		};

		WebAssembly.instantiate(xhr.response, imports).then(function (obj) {
			var module = obj.module;
			var instance = obj.instance;
			exports = instance.exports;
			table = exports.__indirect_function_table;
			exports.main();
			contFunc();
		});
	};
}

function malloc(size) {
	var ret = exports.calloc(1, size);
	if (ret == 0) {
		throw new Error("out of memory");
	}
	return ret;
}

function free(ptr) {
	exports.free(ptr);
}

class FloatVariable {
	constructor(lib) {
		this.lib = lib;
		this.width = lib.width;
		this.ptr = malloc(this.width * 8);
	}

	get() {
		var arr = new Array(this.width);
		var ptr = this.ptr>>3;
		for (var i=0; i<this.width; i++) {
			arr[i] = memF64[ptr++];
		}
		return arr;
	}

	fill(value) {
		var ptr = this.ptr>>3;
		for (var i=0; i<this.width; i++) {
			memF64[ptr++] = value;
		}
	}

	fillInc(value, inc, offset) {
		var ptr = this.ptr>>3;
		for (var i=0; i<this.width; i++) {
			memF64[ptr++] = value + (i+offset)*inc;
		}
	}
	
	add(other) {
		var dest = this.lib.createVariable();
		exports.double_add(this.ptr, other.ptr, dest.ptr, this.width);
		return dest;
	}
	
	sub(other) {
		var dest = this.lib.createVariable();
		exports.double_sub(this.ptr, other.ptr, dest.ptr, this.width);
		return dest;
	}
	
	mul(other) {
		var dest = this.lib.createVariable();
		exports.double_mul(this.ptr, other.ptr, dest.ptr, this.width);
		return dest;
	}
	
	div(other) {
		var dest = this.lib.createVariable();
		exports.double_div(this.ptr, other.ptr, dest.ptr, this.width);
		return dest;
	}

	log() {
		var dest = this.lib.createVariable();
		exports.double_log(this.ptr, dest.ptr, this.width);
		return dest;
	}

	log2() {
		var dest = this.lib.createVariable();
		exports.double_log2(this.ptr, dest.ptr, this.width);
		return dest;
	}

	log10() {
		var dest = this.lib.createVariable();
		exports.double_log10(this.ptr, dest.ptr, this.width);
		return dest;
	}

	exp() {
		var dest = this.lib.createVariable();
		exports.double_exp(this.ptr, dest.ptr, this.width);
		return dest;
	}
	
	pow(other) {
		var dest = this.lib.createVariable();
		exports.double_pow(this.ptr, other.ptr, dest.ptr, this.width);
		return dest;
	}

	sqrt() {
		var dest = this.lib.createVariable();
		exports.double_sqrt(this.ptr, dest.ptr, this.width);
		return dest;
	}

	cbrt() {
		var dest = this.lib.createVariable();
		exports.double_cbrt(this.ptr, dest.ptr, this.width);
		return dest;
	}

	sin() {
		var dest = this.lib.createVariable();
		exports.double_sin(this.ptr, dest.ptr, this.width);
		return dest;
	}

	cos() {
		var dest = this.lib.createVariable();
		exports.double_cos(this.ptr, dest.ptr, this.width);
		return dest;
	}

	tan() {
		var dest = this.lib.createVariable();
		exports.double_tan(this.ptr, dest.ptr, this.width);
		return dest;
	}

	asin() {
		var dest = this.lib.createVariable();
		exports.double_asin(this.ptr, dest.ptr, this.width);
		return dest;
	}

	acos() {
		var dest = this.lib.createVariable();
		exports.double_acos(this.ptr, dest.ptr, this.width);
		return dest;
	}

	atan() {
		var dest = this.lib.createVariable();
		exports.double_atan(this.ptr, dest.ptr, this.width);
		return dest;
	}
	
	min(other) {
		var dest = this.lib.createVariable();
		exports.double_min(this.ptr, other.ptr, dest.ptr, this.width);
		return dest;
	}
	
	max(other) {
		var dest = this.lib.createVariable();
		exports.double_max(this.ptr, other.ptr, dest.ptr, this.width);
		return dest;
	}
}

class FloatLib {
	constructor(width) {
		this.width = width;
		this.variables = [];
	}
	
	createVariable() {
		var variable = new FloatVariable(this);
		this.variables.push(variable);
		return variable;
	}

	destroy() {
		for (var i=0; i<this.variables.length; i++) {
			free(this.variables[i].ptr);
		}
		this.variables.length = 0;
	}
}

class PreciseVariable {
	constructor(lib) {
		this.lib = lib;
		this.width = lib.width;
		this.handle = exports.precise_create_variable(this.width);
	}

	get() {
		var buf = exports.precise_get(this.handle, this.width);
		var arr = new Array(this.width);
		var ptr = buf>>3;
		for (var i=0; i<this.width; i++) {
			arr[i] = memF64[ptr++];
		}
		free(buf);
		return arr;
	}

	fill(value) {
		exports.precise_fill(this.handle, value);
	}

	fillInc(value, inc, offset) {
		//exports.precise_fill_inc(this.handle, value, inc, offset);
		var buf = malloc(this.width * 8);
		var ptr = buf>>3;
		for (var i=0; i<this.width; i++) {
			memF64[ptr++] = value + (i+offset)*inc;
		}
		exports.precise_set(this.handle, this.width, buf);
		free(buf);
	}
	
	add(other) {
		var dest = this.lib.createVariable();
		exports.precise_add(this.handle, other.handle, dest.handle);
		return dest;
	}
	
	sub(other) {
		var dest = this.lib.createVariable();
		exports.precise_sub(this.handle, other.handle, dest.handle);
		return dest;
	}
	
	mul(other) {
		var dest = this.lib.createVariable();
		exports.precise_mul(this.handle, other.handle, dest.handle);
		return dest;
	}
	
	div(other) {
		var dest = this.lib.createVariable();
		exports.precise_div(this.handle, other.handle, dest.handle);
		return dest;
	}
	
	log() {
		var dest = this.lib.createVariable();
		exports.precise_log(this.handle, dest.handle);
		return dest;
	}
	
	log2() {
		var dest = this.lib.createVariable();
		exports.precise_log2(this.handle, dest.handle);
		return dest;
	}
	
	log10() {
		var dest = this.lib.createVariable();
		exports.precise_log10(this.handle, dest.handle);
		return dest;
	}
	
	exp() {
		var dest = this.lib.createVariable();
		exports.precise_exp(this.handle, dest.handle);
		return dest;
	}
	
	pow(other) {
		var dest = this.lib.createVariable();
		exports.precise_pow(this.handle, other.handle, dest.handle);
		return dest;
	}
	
	sqrt() {
		var dest = this.lib.createVariable();
		exports.precise_sqrt(this.handle, dest.handle);
		return dest;
	}
	
	cbrt() {
		var dest = this.lib.createVariable();
		exports.precise_cbrt(this.handle, dest.handle);
		return dest;
	}
	
	sin() {
		var dest = this.lib.createVariable();
		exports.precise_sin(this.handle, dest.handle);
		return dest;
	}
	
	cos() {
		var dest = this.lib.createVariable();
		exports.precise_cos(this.handle, dest.handle);
		return dest;
	}
	
	tan() {
		var dest = this.lib.createVariable();
		exports.precise_tan(this.handle, dest.handle);
		return dest;
	}
	
	asin() {
		var dest = this.lib.createVariable();
		exports.precise_asin(this.handle, dest.handle);
		return dest;
	}
	
	acos() {
		var dest = this.lib.createVariable();
		exports.precise_acos(this.handle, dest.handle);
		return dest;
	}
	
	atan() {
		var dest = this.lib.createVariable();
		exports.precise_atan(this.handle, dest.handle);
		return dest;
	}
	
	min(other) {
		var dest = this.lib.createVariable();
		exports.precise_min(this.handle, other.handle, dest.handle);
		return dest;
	}
	
	max(other) {
		var dest = this.lib.createVariable();
		exports.precise_max(this.handle, other.handle, dest.handle);
		return dest;
	}
}

class PreciseLib {
	constructor(width) {
		this.width = width;
		this.variables = [];
	}
	
	createVariable() {
		var variable = new PreciseVariable(this);
		this.variables.push(variable);
		return variable;
	}

	destroy() {
		for (var i=0; i<this.variables.length; i++) {
			exports.precise_destroy_variable(this.variables[i].handle);
		}
		this.variables.length = 0;
	}
}

class JSVariable {
	constructor(width) {
		this.width = width;
		this.data = new Array(this.width);
	}

	get(variable) {
		var arr = new Array(this.width);
		for (var i=0; i<this.width; i++) {
			arr[i] = this.data[i];
		}
		return arr;
	}

	fill(value) {
		for (var i=0; i<this.width; i++) {
			this.data[i] = value;
		}
	}

	fillInc(value, inc, offset) {
		for (var i=0; i<this.width; i++) {
			this.data[i] = value + (i+offset)*inc;
		}
	}
	
	add(other) {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = this.data[i] + other.data[i];
		}
		return dest;
	}
	
	sub(other) {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = this.data[i] - other.data[i];
		}
		return dest;
	}
	
	mul(other) {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = this.data[i] * other.data[i];
		}
		return dest;
	}
	
	div(other) {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = this.data[i] / other.data[i];
		}
		return dest;
	}
	
	log() {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.log(this.data[i]);
		}
		return dest;
	}
	
	log2() {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.log2(this.data[i]);
		}
		return dest;
	}
	
	log10() {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.log10(this.data[i]);
		}
		return dest;
	}
	
	exp() {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.exp(this.data[i]);
		}
		return dest;
	}
	
	pow(other) {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.pow(this.data[i], other.data[i]);
		}
		return dest;
	}
	
	sqrt() {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.sqrt(this.data[i]);
		}
		return dest;
	}
	
	cbrt() {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.cbrt(this.data[i]);
		}
		return dest;
	}
	
	sin() {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.sin(this.data[i]);
		}
		return dest;
	}
	
	cos() {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.cos(this.data[i]);
		}
		return dest;
	}
	
	tan() {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.tan(this.data[i]);
		}
		return dest;
	}
	
	asin() {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.asin(this.data[i]);
		}
		return dest;
	}
	
	acos() {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.acos(this.data[i]);
		}
		return dest;
	}
	
	atan() {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.atan(this.data[i]);
		}
		return dest;
	}
	
	min(other) {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.min(this.data[i], other.data[i]);
		}
		return dest;
	}
	
	max(other) {
		var dest = new JSVariable(this.width);
		for (var i=0; i<this.width; i++) {
			dest.data[i] = Math.max(this.data[i], other.data[i]);
		}
		return dest;
	}
}

class JSLib {
	constructor(width) {
		this.width = width;
	}
	
	createVariable() {
		return new JSVariable(this.width);
	}

	destroy() {
	}
}

const TOKEN_NUMBER     = 1;
const TOKEN_IDENTIFIER = 2;
const TOKEN_PLUS       = 3;
const TOKEN_MINUS      = 4;
const TOKEN_ASTERISK   = 5;
const TOKEN_SLASH      = 6;
const TOKEN_LPAREN     = 7;
const TOKEN_RPAREN     = 8;
const TOKEN_COMMA      = 9;

class Tokenizer {
	constructor(input) {
		this.input = input;
		this.pos = 0;
		this.again = false;
		this.type = 0;
		this.value = 0;
	}

	next() {
		if (this.again) {
			this.again = false;
			return this.type != 0;
		}
		
		var c = 0;
		do {
			if (this.pos >= this.input.length) {
				return false;
			}
			c = this.input.charCodeAt(this.pos++);
		}
		while (c == 32); // ' '

		if (c >= 48 && c <= 57) { // '0' .. '9'
			var start = this.pos-1;
			while (this.pos < this.input.length) {
				c = this.input.charCodeAt(this.pos++);
				if (c >= 48 && c <= 57) { // '0' .. '9'
					continue;
				}
				else {
					this.pos--;
					break;
				}
			}
			if (this.pos < this.input.length && this.input.charCodeAt(this.pos) == 46) { // '.'
				this.pos++;
				while (this.pos < this.input.length) {
					c = this.input.charCodeAt(this.pos++);
					if (c >= 48 && c <= 57) { // '0' .. '9'
						continue;
					}
					else {
						this.pos--;
						break;
					}
				}
			}
			if (this.pos < this.input.length && (this.input.charCodeAt(this.pos) == 101 || this.input.charCodeAt(this.pos) == 69)) { // 'e' or 'E'
				this.pos++;
				if (this.pos < this.input.length) {
					c = this.input.charCodeAt(this.pos);
					if (c == 43 || c == 45) { // '+' or '-'
						this.pos++;
					}
				}
				while (this.pos < this.input.length) {
					c = this.input.charCodeAt(this.pos++);
					if (c >= 48 && c <= 57) { // '0' .. '9'
						continue;
					}
					else {
						this.pos--;
						break;
					}
				}
			}
			this.type = TOKEN_NUMBER;
			this.value = parseFloat(this.input.substring(start, this.pos));
			return true;
		}
		else if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 95) { // 'A'..'Z' || 'a'..'z' || '_'
			var start = this.pos-1;
			while (this.pos < this.input.length) {
				c = this.input.charCodeAt(this.pos++);
				if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 95 || (c >= 48 && c <= 57)) { // 'A'..'Z' || 'a'..'z' || '_' || '0'..'9'
					continue;
				}
				else {
					this.pos--;
					break;
				}
			}
			this.type = TOKEN_IDENTIFIER;
			this.value = this.input.substring(start, this.pos);
			return true;
		}
		else if (c == 43) { // '+'
			this.type = TOKEN_PLUS;
			this.value = 0;
			return true;
		}
		else if (c == 45) { // '-'
			this.type = TOKEN_MINUS;
			this.value = 0;
			return true;
		}
		else if (c == 42) { // '*'
			this.type = TOKEN_ASTERISK;
			this.value = 0;
			return true;
		}
		else if (c == 47) { // '/'
			this.type = TOKEN_SLASH;
			this.value = 0;
			return true;
		}
		else if (c == 40) { // '('
			this.type = TOKEN_LPAREN;
			this.value = 0;
			return true;
		}
		else if (c == 41) { // ')'
			this.type = TOKEN_RPAREN;
			this.value = 0;
			return true;
		}
		else if (c == 44) { // ','
			this.type = TOKEN_COMMA;
			this.value = 0;
			return true;
		}
		else {
			throw new Error("syntax error at "+(this.pos-1));
		}
		this.type = 0;
		this.value = 0;
		return false;
	}

	back() {
		this.again = true;
	}

	expectNext(s) {
		if (!this.next()) {
			throw new Error("expected "+s);
		}
	}

	expectType(type, s) {
		if (!this.next() || this.type != type) {
			throw new Error("expected "+s);
		}
	}
}

var functions = {
	"log": function(param) { return param.log(); },
	"log2": function(param) { return param.log2(); },
	"log10": function(param) { return param.log10(); },
	"exp": function(param) { return param.exp(); },
	"sqrt": function(param) { return param.sqrt(); },
	"cbrt": function(param) { return param.cbrt(); },
	"sin": function(param) { return param.sin(); },
	"cos": function(param) { return param.cos(); },
	"tan": function(param) { return param.tan(); },
	"asin": function(param) { return param.asin(); },
	"acos": function(param) { return param.acos(); },
	"atan": function(param) { return param.atan(); }
};

var functions2 = {
	"pow": function(param1, param2) { return param1.pow(param2); },
	"min": function(param1, param2) { return param1.min(param2); },
	"max": function(param1, param2) { return param1.max(param2); }
};

function evalExpression(ops, indexVar, input) {
	var tok = new Tokenizer(input);

	function parsePrimary() {
		tok.expectNext("number, variable or math function");
		if (tok.type == TOKEN_PLUS || tok.type == TOKEN_MINUS) {
			var minus = tok.type == TOKEN_MINUS;
			tok.expectNext("number");
			if (tok.type == TOKEN_NUMBER) {
				var variable = ops.createVariable();
				variable.fill(minus? -tok.value : tok.value);
				return variable;
			}
			else {
				tok.back();
				var ret = parsePrimary();
				if (minus) {
					var tmp = ops.createVariable();
					tmp.fill(0);
					ret = tmp.sub(ret);
				}
				return ret;
			}
		}
		else if (tok.type == TOKEN_NUMBER) {
			var variable = ops.createVariable();
			variable.fill(tok.value);
			return variable;
		}
		else if (tok.type == TOKEN_IDENTIFIER) {
			var func = functions[tok.value];
			if (func) {
				tok.expectType(TOKEN_LPAREN, "(");
				var param = parseExpression();
				tok.expectType(TOKEN_RPAREN, ")");
				return func(param);
			}
			var func2 = functions2[tok.value];
			if (func2) {
				tok.expectType(TOKEN_LPAREN, "(");
				var param1 = parseExpression();
				tok.expectType(TOKEN_COMMA, ",");
				var param2 = parseExpression();
				tok.expectType(TOKEN_RPAREN, ")");
				return func2(param1, param2);
			}
			if (tok.value == "x") {
				return indexVar;
			}
			throw new Error("expected variable or math function");
		}
		else if (tok.type == TOKEN_LPAREN) {
			var param = parseExpression();
			tok.expectType(TOKEN_RPAREN, ")");
			return param;
		}
		else {
			throw new Error("number, variable or math function");
		}
	}

	function parseMultiplicative() {
		var value = parsePrimary();
		while (tok.next()) {
			if (tok.type == TOKEN_ASTERISK) {
				var other = parsePrimary();
				value = value.mul(other);
			}
			else if (tok.type == TOKEN_SLASH) {
				var other = parsePrimary();
				value = value.div(other);
			}
			else {
				tok.back();
				break;
			}
		}
		return value;
	}

	function parseAdditive() {
		var value = parseMultiplicative();
		while (tok.next()) {
			if (tok.type == TOKEN_PLUS) {
				var other = parseMultiplicative();
				value = value.add(other);
			}
			else if (tok.type == TOKEN_MINUS) {
				var other = parseMultiplicative();
				value = value.sub(other);
			}
			else {
				tok.back();
				break;
			}
		}
		return value;
	}

	function parseExpression() {
		return parseAdditive();
	}

	var value = parseExpression();
	if (tok.next()) {
		throw new Error("unexpected tokens at end");
	}
	return value;
}

function start() {
	demoElement.style.backgroundColor = "#CCCCCC";
	demoElement.style.position = "relative";

	var rightControls = document.createElement("div");
	rightControls.style.padding = "5px";
	rightControls.style.width = "150px";
	rightControls.style.float = "right";
	demoElement.appendChild(rightControls);

	var ratio = window.devicePixelRatio;
	var totalWidth = 600 * ratio;
	var totalHeight = 250 * ratio;
	canvas = document.createElement("canvas");
	canvas.width = totalWidth;
	canvas.height = totalHeight;
	canvas.style = "width: "+(totalWidth/ratio)+"px; height: "+(totalHeight/ratio)+"px";
	ctx = canvas.getContext("2d", { "alpha": false });
	demoElement.appendChild(canvas);

	ctx.fillStyle = "#FFFFFF";
	ctx.fillRect(0, 0, totalWidth, totalHeight);

	var controls = document.createElement("div");
	controls.style.padding = "5px";
	demoElement.appendChild(controls);

	var numberInput = document.createElement("input");
	numberInput.type = "number";
	numberInput.style.width = "50px";
	numberInput.style.marginRight = "5px";
	numberInput.value = 2;
	numberInput.min = 1;
	numberInput.max = 128;
	numberInput.title = "Number of calculations done at once for the reference implementation (increasing it makes it faster but also more laggy).";
	controls.appendChild(numberInput);

	var startButton = document.createElement("button");
	startButton.innerHTML = "Start";
	startButton.style.marginRight = "5px";
	controls.appendChild(startButton);

	var stopButton = document.createElement("button");
	stopButton.innerHTML = "Stop";
	stopButton.style.marginRight = "5px";
	stopButton.disabled = true;
	controls.appendChild(stopButton);

	var label = document.createElement("span");
	label.style.fontFamily = "sans-serif";
	label.style.fontSize = "12px";
	controls.appendChild(label);

	var label2 = document.createElement("span");
	label2.style.fontFamily = "sans-serif";
	label2.style.fontSize = "12px";
	label2.style.position = "absolute";
	label2.style.marginTop = "3px";
	label2.style.right = "10px";
	controls.appendChild(label2);

	function createCheckbox(parent, text, radioName) {
		var label = document.createElement("label");
		label.innerHTML = text;
		label.style.display = "block";
		label.style.fontFamily = "sans-serif";
		label.style.fontSize = "12px";
		label.style.userSelect = "none";
		var checkbox = document.createElement("input");
		if (radioName !== undefined) {
			checkbox.type = "radio";
			checkbox.name = radioName;
		}
		else {
			checkbox.type = "checkbox";
		}
		checkbox.style.marginRight = "5px";
		label.insertBefore(checkbox, label.firstChild);
		parent.appendChild(label);
		return checkbox;
	}

	var floatLibCheckbox = createCheckbox(rightControls, "MathLib");
	var jsLibCheckbox = createCheckbox(rightControls, "JavaScript");
	var preciseLibCheckbox = createCheckbox(rightControls, "reference (slow)");
	preciseLibCheckbox.parentNode.style.marginTop = "10px";

	floatLibCheckbox.checked = true;
	preciseLibCheckbox.checked = true;

	floatLibCheckbox.addEventListener("change", function() {
		if (floatLibCheckbox.checked && jsLibCheckbox.checked) {
			jsLibCheckbox.checked = false;
		}
		repaint();
	});
	jsLibCheckbox.addEventListener("change", function() {
		if (jsLibCheckbox.checked && floatLibCheckbox.checked) {
			floatLibCheckbox.checked = false;
		}
		repaint();
	});
	preciseLibCheckbox.addEventListener("change", function() {
		restart();
	});

	expressionInput = document.createElement("input");
	expressionInput.type = "text";
	expressionInput.style.width = "100%";
	expressionInput.style.marginTop = "10px";
	expressionInput.style.boxSizing = "border-box";
	expressionInput.value = "sin(x)";
	controls.appendChild(expressionInput);

	var values1 = [];
	var values2 = [];
	var values3 = [];
	var showMaxError = -1;

	function updateMaxError(maxError) {
		if (maxError != showMaxError) {
			if (maxError == -1) {
				label2.innerHTML = "";
			}
			else {
				label2.innerHTML = "&nbsp;Max. error: "+maxError;
			}
			showMaxError = maxError;
		}
	}

	function repaint() {
		ctx.fillStyle = "#FFFFFF";
		ctx.fillRect(0, 0, totalWidth, totalHeight);

		var width = totalWidth;
		var height = 200 * ratio;

		ctx.strokeStyle = "#CCC";
		ctx.lineWidth = 1 * ratio;
		ctx.beginPath();
		ctx.moveTo(0*ratio, height);
		ctx.lineTo(width, height);
		ctx.stroke();

		ctx.save();
		ctx.beginPath();
		ctx.rect(0, 0, width, height);
		ctx.clip();

		var scaleY = 30;

		ctx.strokeStyle = "#CCC";
		ctx.lineWidth = 1 * ratio;
		ctx.beginPath();
		ctx.moveTo(0*ratio, height*0.5);
		ctx.lineTo(width, height*0.5);
		ctx.stroke();
		ctx.beginPath();
		ctx.moveTo(0*ratio, height*0.5 - 1*scaleY*ratio);
		ctx.lineTo(width, height*0.5 - 1*scaleY*ratio);
		ctx.stroke();
		ctx.beginPath();
		ctx.moveTo(0*ratio, height*0.5 + 1*scaleY*ratio);
		ctx.lineTo(width, height*0.5 + 1*scaleY*ratio);
		ctx.stroke();
		ctx.beginPath();
		ctx.moveTo(width*0.5*ratio, 0);
		ctx.lineTo(width*0.5*ratio, height);
		ctx.stroke();

		function drawValues(values) {
			if (values.length == 0) return;
			ctx.beginPath();
			ctx.moveTo(0*ratio, height*0.5 - values[0]*scaleY*ratio);
			for (var i=1; i<values.length; i++) {
				var value = values[i];
				if (!isFinite(value)) {
					ctx.stroke();
					ctx.beginPath();
					i++;
					while (i < values.length) {
						value = values[i];
						if (isFinite(value)) {
							ctx.moveTo(i*ratio, height*0.5 - value*scaleY*ratio);
							break;
						}
						i++;
					}
				}
				else {
					ctx.lineTo(i*ratio, height*0.5 - value*scaleY*ratio);
				}
			}
			ctx.stroke();
		}

		ctx.lineWidth = 2 * ratio;
		ctx.strokeStyle = "#00A";

		if (!floatLibCheckbox.checked && !jsLibCheckbox.checked) {
			drawValues(values1);
		}

		if (floatLibCheckbox.checked) {
			drawValues(values2);
		}
		if (jsLibCheckbox.checked) {
			drawValues(values3);
		}

		ctx.restore();

		ctx.save();
		ctx.beginPath();
		ctx.rect(0, height, width, totalHeight - height);
		ctx.clip();
		ctx.translate(0, height);

		height = 50 * ratio;

		function drawRelativeError(values, reference) {
			if (values.length == 0 || reference.length == 0) return;
			var len = Math.min(values.length, reference.length);
			var commonLen = Math.min(len, Math.min(values2.length, values3.length));
			var maxError2 = 0;
			var maxError3 = 0;
			for (var i=0; i<commonLen; i++) {
				var value2 = values2[i];
				var value3 = values3[i];
				var ref = reference[i];
				if (!isFinite(ref)) continue;
				if (isFinite(value2)) {
					var diff = Math.abs(value2 - ref);
					maxError2 = Math.max(maxError2, diff);
				}
				if (isFinite(value3)) {
					var diff = Math.abs(value3 - ref);
					maxError3 = Math.max(maxError3, diff);
				}
			}

			if (floatLibCheckbox.checked) {
				updateMaxError(maxError2);
			}
			else if (jsLibCheckbox.checked) {
				updateMaxError(maxError3);
			}
			else {
				updateMaxError(-1);
			}

			var maxError = Math.max(maxError2, maxError3);
			var scale = height / maxError;

			ctx.beginPath();
			ctx.moveTo(0*ratio, height - Math.abs(values[0] - reference[0])*scale);
			for (var i=1; i<len; i++) {
				var value = values[i];
				var ref = reference[i];
				if (!isFinite(value) || !isFinite(ref)) {
					ctx.stroke();
					ctx.beginPath();
					i++;
					while (i < values.length) {
						value = values[i];
						var ref = reference[i];
						if (isFinite(value) && isFinite(ref)) {
							var diff = Math.abs(value - ref);
							ctx.moveTo(i*ratio, height - diff*scale);
							break;
						}
						i++;
					}
				}
				else {
					var diff = Math.abs(value - ref);
					ctx.lineTo(i*ratio, height - diff*scale);
				}
			}
			ctx.stroke();
		}

		ctx.lineWidth = 1 * ratio;
		ctx.strokeStyle = "#A00";

		if (floatLibCheckbox.checked && preciseLibCheckbox.checked) {
			drawRelativeError(values2, values1);
		}
		else if (jsLibCheckbox.checked && preciseLibCheckbox.checked) {
			drawRelativeError(values3, values1);
		}

		ctx.restore();
	}

	window.addEventListener("resize", function () {
		ratio = window.devicePixelRatio;
		totalWidth = 600 * ratio;
		totalHeight = 250 * ratio;
		canvas.width = totalWidth;
		canvas.height = totalHeight;
		canvas.style = "width: "+(totalWidth/ratio)+"px; height: "+(totalHeight/ratio)+"px";
		ctx = canvas.getContext("2d", { "alpha": false });
		repaint();
	});

	var inputString = null;
	var errorShown = false;

	function testOps(ops, start, values, contFunc) {
		var idx = ops.createVariable();
		idx.fillInc(-30, 0.1, start);
		
		var result = null;
		try {
			result = evalExpression(ops, idx, inputString).get();
		}
		catch (e) {
			if (!errorShown) {
				console.log(e.message);
				errorShown = true;
				stopProcessing = true;
				contFunc();
				return;
			}
			result = ops.createVariable().get();
		}
		
		for (var i=0; i<result.length; i++) {
			values.push(result[i]);
		}

		ops.destroy();
		repaint();
		if (contFunc) {
			setTimeout(contFunc, 0);
		}
	}

	var stopProcessing = false;

	function testOpsSlow(ops, totalSize, values, contFunc) {
		var off = 0;
		var cnt = totalSize / ops.width;

		function draw() {
			if (stopProcessing) {
				contFunc();
				return;
			}
			testOps(ops, off*ops.width, values, ++off < cnt? draw : contFunc);
		}
		draw();
	}

	function runPlot() {
		updateMaxError(-1);

		var numSelected = 2;
		if (preciseLibCheckbox.checked) numSelected++;
		if (numSelected == 0) {
			values1.length = 0;
			values2.length = 0;
			values3.length = 0;
			repaint();
			return;
		}

		numberInput.disabled = true;
		startButton.disabled = true;
		stopButton.disabled = false;
		inputString = expressionInput.value;
		errorShown = false;

		var startTime = performance.now();
		label.innerHTML = "Processing...";

		var counter = 0;
		function finish() {
			if (++counter == numSelected) {
				numberInput.disabled = false;
				startButton.disabled = false;
				stopButton.disabled = true;
				stopProcessing = false;
				
				var endTime = performance.now();
				label.innerHTML = "Took "+Math.round(endTime - startTime)+" ms";
			}
		}

		values1.length = 0;
		values2.length = 0;
		values3.length = 0;
		repaint();

		var width = totalWidth;

		if (preciseLibCheckbox.checked) {
			testOpsSlow(new PreciseLib(numberInput.value), Math.ceil(width/ratio), values1, finish);
		}
		testOpsSlow(new FloatLib(256), Math.ceil(width/ratio), values2, finish);
		testOpsSlow(new JSLib(256), Math.ceil(width/ratio), values3, finish);
	}

	startButton.addEventListener("click", function() {
		runPlot();
	});

	stopButton.addEventListener("click", function() {
		stopProcessing = true;
	});

	function restart() {
		if (startButton.disabled) {
			stopProcessing = true;
			var timer = null;
			function checkStopped() {
				if (!stopProcessing) {
					clearTimeout(timer);
					if (!startButton.disabled) {
						runPlot();
					}
				}
			}
			timer = setInterval(checkStopped, 50);
		}
		else {
			runPlot();
		}
	}
	restartFunc = restart;

	expressionInput.addEventListener("keydown", function (e) {
		if (e.keyCode == 13) {
			restart();
		}
	});

	if ("IntersectionObserver" in window) {
		var observer = new IntersectionObserver(function(entries, observer) {
			if (entries.length > 0 && entries[0].isIntersecting) {
				observer.unobserve(demoElement);
				if (!startButton.disabled) {
					runPlot();
				}
			}
		}, {
			root: null,
			threshold: 0.25
		});
		observer.observe(demoElement);
	}

	startConvertor();
}

function startConvertor() {
	convertorElement.style.backgroundColor = "#CCCCCC";
	convertorElement.style.position = "relative";

	var controls = document.createElement("div");
	controls.style.padding = "5px";
	convertorElement.appendChild(controls);

	var table = document.createElement("table");
	controls.appendChild(table);

	function addRow(label, elem) {
		var row = document.createElement("tr");
		var cell = document.createElement("td");
		cell.innerHTML = label;
		row.appendChild(cell);
		cell = document.createElement("td");
		cell.appendChild(elem);
		row.appendChild(cell);
		table.appendChild(row);
		return cell;
	}

	var decimalInput = document.createElement("input");
	decimalInput.type = "text";
	decimalInput.style.width = "160px";
	decimalInput.style.marginRight = "5px";
	decimalInput.value = "123.456";
	var cell = addRow("Decimal input (MathLib):", decimalInput);

	var decimalSetButton = document.createElement("button");
	decimalSetButton.innerHTML = "Set";
	decimalSetButton.style.marginLeft = "2px";
	decimalSetButton.style.width = "3em";
	cell.appendChild(decimalSetButton);

	var hexInput = document.createElement("input");
	hexInput.type = "text";
	hexInput.style.width = "160px";
	hexInput.style.marginRight = "5px";
	hexInput.value = "";
	cell = addRow("Hex input/output:", hexInput);

	var hexSetButton = document.createElement("button");
	hexSetButton.innerHTML = "Set";
	hexSetButton.style.marginLeft = "2px";
	hexSetButton.style.width = "3em";
	cell.appendChild(hexSetButton);

	var incButton = document.createElement("button");
	incButton.innerHTML = "+";
	incButton.style.marginLeft = "5px";
	incButton.style.width = "2em";
	cell.appendChild(incButton);

	var decButton = document.createElement("button");
	decButton.innerHTML = "-";
	decButton.style.marginLeft = "2px";
	decButton.style.width = "2em";
	cell.appendChild(decButton);

	var backInput = document.createElement("input");
	backInput.type = "text";
	backInput.style.width = "160px";
	backInput.style.marginRight = "5px";
	backInput.value = "";
	backInput.readOnly = true;
	addRow("Conversion back (MathLib):", backInput);

	var backInput2 = document.createElement("input");
	backInput2.type = "text";
	backInput2.style.width = "160px";
	backInput2.style.marginRight = "5px";
	backInput2.value = "";
	backInput2.readOnly = true;
	addRow("Conversion back (JS):", backInput2);

	function stringToDouble(input) {
		var str = malloc(input.length+1);
		for (var i=0; i<input.length; i++) {
			mem8[str + i] = input.charCodeAt(i);
		}
		mem8[str + input.length] = 0;
		var ret = exports.string_to_double(str);
		free(str);
		return ret;
	}

	function doubleToString(input) {
		var buf = malloc(32);
		var ret = exports.double_to_string(buf, input);
		var s = [], len=0, ptr=ret;
		for (;;) {
			var c = mem8[ptr++];
			if (c == 0) break;
			s[len++] = c;
		}
		free(buf);
		return String.fromCharCode.apply(null, s);
	}

	function padLeft(s, len, c) {
		while (s.length < len) {
			s = c + s;
		}
		return s;
	}

	function convertFromDecimal() {
		var value = stringToDouble(decimalInput.value);
		var buf = malloc(8);
		memF64[buf>>3] = value;
		var lo = mem32[buf>>2];
		var hi = mem32[(buf+4)>>2];
		free(buf);
		if (hi < 0) hi = 0xFFFFFFFF + hi + 1;
		if (lo < 0) lo = 0xFFFFFFFF + lo + 1;
		hexInput.value = (padLeft(hi.toString(16), 8, "0")+padLeft(lo.toString(16), 8, "0")).toUpperCase();
		backInput.value = doubleToString(value);
		backInput2.value = value.toString();
	}

	function convertFromHex() {
		var hex = padLeft(hexInput.value, 16, "0");
		if (hex.length > 16) return;
		var hi = parseInt(hex.substring(0, 8), 16);
		var lo = parseInt(hex.substring(8, 16), 16);
		var buf = malloc(8);
		mem32[buf>>2] = lo;
		mem32[(buf+4)>>2] = hi;
		var value = memF64[buf>>3];
		free(buf);
		backInput.value = doubleToString(value);
		backInput2.value = value.toString();
	}

	decimalInput.addEventListener("keydown", function (e) {
		if (e.keyCode == 13) {
			convertFromDecimal();
		}
	});

	hexInput.addEventListener("keydown", function (e) {
		if (e.keyCode == 13) {
			convertFromHex();
		}
	});

	decimalSetButton.addEventListener("click", function () {
		convertFromDecimal();
	});

	hexSetButton.addEventListener("click", function () {
		convertFromHex();
	});

	incButton.addEventListener("click", function () {
		var hex = padLeft(hexInput.value, 16, "0");
		if (hex.length > 16) return;
		var hi = parseInt(hex.substring(0, 8), 16);
		var lo = parseInt(hex.substring(8, 16), 16);
		lo = (lo+1)|0;
		if (lo < 0) lo = 0xFFFFFFFF + lo + 1;
		if (lo == 0) {
			hi = (hi+1)|0;
		}
		if (hi < 0) hi = 0xFFFFFFFF + hi + 1;
		hexInput.value = (padLeft(hi.toString(16), 8, "0")+padLeft(lo.toString(16), 8, "0")).toUpperCase();
		var buf = malloc(8);
		mem32[buf>>2] = lo;
		mem32[(buf+4)>>2] = hi;
		var value = memF64[buf>>3];
		free(buf);
		backInput.value = doubleToString(value);
		backInput2.value = value.toString();
	});

	decButton.addEventListener("click", function () {
		var hex = padLeft(hexInput.value, 16, "0");
		if (hex.length > 16) return;
		var hi = parseInt(hex.substring(0, 8), 16);
		var lo = parseInt(hex.substring(8, 16), 16);
		lo = (lo-1)|0;
		if (lo < 0) lo = 0xFFFFFFFF + lo + 1;
		if (lo == 0xFFFFFFFF) {
			hi = (hi-1)|0;
		}
		if (hi < 0) hi = 0xFFFFFFFF + hi + 1;
		hexInput.value = (padLeft(hi.toString(16), 8, "0")+padLeft(lo.toString(16), 8, "0")).toUpperCase();
		var buf = malloc(8);
		mem32[buf>>2] = lo;
		mem32[(buf+4)>>2] = hi;
		var value = memF64[buf>>3];
		free(buf);
		backInput.value = doubleToString(value);
		backInput2.value = value.toString();
	});

	convertFromDecimal();
}

function runDemo(elemId, convertorId) {
	document.addEventListener("DOMContentLoaded", function () {
		demoElement = document.getElementById(elemId);
		convertorElement = document.getElementById(convertorId);
		loadWASM(start);
	});
}

function setExpression(expr) {
	if (!expressionInput) return;
	expressionInput.value = expr;
	restartFunc();
}

/*
 * app.js - the script of rsswitch-web: shows the rooms and sockets of
 * the JSON API, switches a socket when On or Off is tapped, and in edit
 * mode adds, changes and removes them.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Names and messages come from the data file and from rsswitchd. They
 * reach the page only as text (textContent and text nodes), never
 * through an HTML parser.
 */

"use strict";

const HINT = "This is the last command sent, not the real state of the "
	+ "socket.";

/* What the socket type select shows. A type that is not here shows its
 * id, so a type that the server learns needs no change of the page. */
const TYPE_LABELS = {
	"rev_008345": "REV 008345",
	"pollin_2605": "Pollin 2605",
};

/* The element that shows the error of each field of the socket form. */
const FIELD_ERRORS = {
	name: "socket-name-error",
	type: "socket-type-error",
	group: "socket-group-error",
	number: "socket-number-error",
	room_id: "socket-room-error",
};

/*
 * What the page remembers besides the rooms. render() reads all of it,
 * so a rebuilt page keeps the rows that are in flight and the messages
 * under the rows.
 *   tree     the answer of GET /api/rooms, or null before the first one
 *   pending  socket id -> "on" or "off" for each switch request in flight
 *   notes    socket id -> {kind: "error" | "info", text}
 *   flash    ids of the rows that flash after a successful switch
 *   busyAllOff  room ids, and HOUSEHOLD, of the All-off requests in flight
 *   loadSeq  number of the newest request for the rooms
 *   edit     true while the page is in edit mode
 *   meta     the "socket_types" of GET /api/meta, or null before it loads
 *   busy     true while a management request is in flight; every
 *            management button is disabled meanwhile
 *   roomNotes   room id -> text of the refusal shown on that room card
 *   editedRoom  id of the room the room form renames, or null to add one
 *   editedSocket  the socket the socket form edits, or null to add one
 */
const HOUSEHOLD = "*";

const ui = {
	tree: null,
	pending: new Map(),
	notes: new Map(),
	flash: new Set(),
	busyAllOff: new Set(),
	loadSeq: 0,
	loadFailed: false,
	edit: false,
	meta: null,
	metaFailed: false,
	busy: false,
	roomNotes: new Map(),
	editedRoom: null,
	editedSocket: null,
};

/* Build an element. Text goes in as text, never as markup. */
function el(tag, props, ...children) {
	const node = document.createElement(tag);
	for (const [key, value] of Object.entries(props || {})) {
		if (key === "class") {
			node.className = value;
		} else if (key === "text") {
			node.textContent = value;
		} else if (key === "disabled") {
			node.disabled = Boolean(value);
		} else {
			node.setAttribute(key, value);
		}
	}
	node.append(...children);
	return node;
}

/*
 * The only caller of the browser's request function. A POST always
 * says it is JSON, with an empty object when there is no body: the app
 * answers 415 to anything else.
 *
 * It returns {ok: true, status, data} or {ok: false, status, error,
 * retryAfter}. A request that does not get an answer is the error
 * "network".
 */
async function api(method, path, body) {
	const init = {method: method, cache: "no-store", headers: {}};
	if (method === "POST" || method === "PUT" || method === "PATCH") {
		init.headers["Content-Type"] = "application/json";
		init.body = JSON.stringify(body || {});
	}
	let resp;
	try {
		resp = await fetch(path, init);
	} catch (e) {
		return {ok: false, status: 0, error: {
			code: "network", message: "web app unreachable"}};
	}
	if (resp.status === 204) {
		return {ok: true, status: 204, data: null};
	}
	let data = null;
	try {
		data = await resp.json();
	} catch (e) {
		/* An answer that is not JSON, such as one from a proxy. */
	}
	if (resp.ok) {
		return {ok: true, status: resp.status, data: data};
	}
	const error = (data && data.error) || {
		code: "http_error", message: "HTTP " + resp.status};
	return {ok: false, status: resp.status, error: error,
		retryAfter: resp.headers.get("Retry-After")};
}

/* The text that tells the owner why a request failed. */
function errorText(error, retryAfter) {
	switch (error.code) {
	case "daemon_unreachable":
		return "rsswitchd unreachable";
	case "daemon_timeout":
		return "rsswitchd timed out";
	case "daemon_error":
		return "rsswitchd: " + (error.daemon_message || error.message);
	case "network":
		return "web app unreachable";
	case "too_many_attempts":
		return "too many failed logins, try again "
			+ (retryAfter ? "in " + retryAfter + " s" : "later");
	case "unauthorized":
		return "login needed, reload the page";
	default:
		return error.message || error.code;
	}
}

/* "Never sent", or "Last sent: On · 07:12", with the date in front
 * of the time when the send was not today. */
function formatLastSent(lastSent) {
	if (!lastSent) {
		return "Never sent";
	}
	const label = lastSent.state === "on" ? "On" : "Off";
	const at = new Date(lastSent.at);
	if (isNaN(at.getTime())) {
		return "Last sent: " + label;
	}
	let when = at.toLocaleTimeString([], {
		hour: "2-digit", minute: "2-digit"});
	if (at.toDateString() !== new Date().toDateString()) {
		when = at.toLocaleDateString() + " " + when;
	}
	return "Last sent: " + label + " · " + when;
}

function showBanner(text, kind) {
	const banner = document.getElementById("banner");
	ui.loadFailed = false;
	banner.textContent = text;
	banner.className = kind === "error" ? "banner banner-error" : "banner";
	banner.hidden = false;
}

function hideBanner() {
	const banner = document.getElementById("banner");
	banner.textContent = "";
	banner.hidden = true;
}

/* Forget what is left of sockets and rooms that are no longer in the tree. A
 * request in flight removes its own entry when it ends. */
function dropGoneSockets() {
	const ids = new Set();
	const roomIds = new Set();
	for (const room of ui.tree.rooms) {
		roomIds.add(room.id);
		for (const sock of room.sockets) {
			ids.add(sock.id);
		}
	}
	for (const id of Array.from(ui.roomNotes.keys())) {
		if (!roomIds.has(id)) {
			ui.roomNotes.delete(id);
		}
	}
	for (const id of Array.from(ui.notes.keys())) {
		if (!ids.has(id)) {
			ui.notes.delete(id);
		}
	}
	for (const id of Array.from(ui.flash)) {
		if (!ids.has(id)) {
			ui.flash.delete(id);
		}
	}
}

/* Fetch the rooms and show them. An answer older than the newest
 * request is dropped. */
async function loadTree() {
	const seq = ++ui.loadSeq;
	const res = await api("GET", "/api/rooms");
	if (seq !== ui.loadSeq) {
		return;
	}
	if (!res.ok || !res.data || !Array.isArray(res.data.rooms)) {
		const error = res.ok
			? {code: "http_error", message: "unexpected answer"}
			: res.error;
		showBanner(errorText(error, res.retryAfter), "error");
		/* After showBanner(), which clears the flag: only a banner
		 * that reports a failed load goes away with the next load. */
		ui.loadFailed = true;
		return;
	}
	if (ui.loadFailed) {
		hideBanner();
		ui.loadFailed = false;
	}
	ui.tree = res.data;
	dropGoneSockets();
	render();
}

function renderSocket(sock, index, room) {
	const wanted = ui.pending.get(sock.id);
	const busy = wanted !== undefined;
	const note = ui.notes.get(sock.id);
	const last = el("span", {
		class: "last-sent", title: HINT, text: formatLastSent(sock.last_sent)});
	const buttons = [["on", "On"], ["off", "Off"]].map(([state, label]) => {
		const sending = busy && wanted === state;
		const button = el("button", {
			class: state + (sending ? " sending" : ""),
			type: "button",
			disabled: busy,
			"aria-label": "Switch " + sock.name + " " + label.toLowerCase(),
			text: sending ? "Sending…" : label});
		button.addEventListener("click", () => switchSocket(sock.id, state));
		return button;
	});
	const row = el("div", {
		class: "socket" + (ui.flash.has(sock.id) ? " flash" : "")},
		el("div", {class: "socket-info"},
			el("span", {class: "socket-name", text: sock.name}), last));
	if (!ui.edit) {
		row.append(el("div", {class: "switch"}, ...buttons));
	} else {
		const edit = el("button", {
			type: "button", disabled: ui.busy, text: "\u270E",
			"aria-label": "Edit " + sock.name});
		edit.addEventListener("click", () => openSocketDialog(
			sock.room_id, sock));
		const remove = el("button", {
			type: "button", disabled: ui.busy, text: "\u{1F5D1}\uFE0E",
			"aria-label": "Delete " + sock.name});
		remove.addEventListener("click", () => deleteSocket(sock));
		const arrows = arrowButtons(sock.name, index, room.sockets.length,
			(delta) => moveSocket(room, index, delta));
		row.append(el("div", {class: "edit-controls"}, ...arrows, edit,
			remove));
	}
	if (note) {
		row.append(el("p", {
			class: "note note-" + note.kind, text: note.text}));
	}
	return row;
}

function renderRoom(room, index) {
	const sockets = room.sockets.map(
		(sock, at) => renderSocket(sock, at, room));
	if (sockets.length === 0) {
		sockets.push(el("p", {class: "hint", text: "No sockets yet."}));
	}
	const head = el("div", {class: "room-head"},
		el("h2", {text: room.name}));
	if (!ui.edit) {
		const busy = ui.busyAllOff.has(room.id);
		const allOffButton = el("button", {
			class: "all-off" + (busy ? " sending" : ""),
			type: "button",
			disabled: busy || room.sockets.length === 0,
			"aria-label": "Turn off every socket in " + room.name,
			text: busy ? "Sending\u2026" : "All off"});
		allOffButton.addEventListener("click", () => allOff(room.id));
		head.append(allOffButton);
	} else {
		const rename = el("button", {
			type: "button", disabled: ui.busy, text: "\u270E",
			"aria-label": "Rename " + room.name});
		rename.addEventListener("click", () => openRoomDialog(room));
		const remove = el("button", {
			type: "button", disabled: ui.busy, text: "\u{1F5D1}\uFE0E",
			"aria-label": "Delete " + room.name});
		remove.addEventListener("click", () => deleteRoom(room));
		const arrows = arrowButtons(room.name, index, ui.tree.rooms.length,
			(delta) => moveRoom(index, delta));
		head.append(el("div", {class: "edit-controls"}, ...arrows, rename,
			remove));
		const add = el("button", {
			class: "add-socket", type: "button", disabled: ui.busy,
			"aria-label": "Add a socket to " + room.name,
			text: "+ Socket"});
		add.addEventListener("click", () => openSocketDialog(room.id));
		sockets.push(add);
	}
	const parts = [head];
	const note = ui.roomNotes.get(room.id);
	if (note) {
		parts.push(el("p", {class: "card-note", text: note}));
	}
	return el("section", {class: "room"}, ...parts, ...sockets);
}

function countSockets() {
	let count = 0;
	for (const room of ui.tree ? ui.tree.rooms : []) {
		count += room.sockets.length;
	}
	return count;
}

/* The All-off button of the header lives in the HTML. */
function renderHouseholdButton() {
	const button = document.getElementById("all-off");
	const busy = ui.busyAllOff.has(HOUSEHOLD);
	button.disabled = busy || countSockets() === 0;
	button.textContent = busy ? "Sending\u2026" : "All off";
	button.classList.toggle("sending", busy);
}

/* The Edit button waits for the limits of the socket types. */
function renderEditButton() {
	const button = document.getElementById("edit-toggle");
	button.disabled = ui.meta === null;
	button.textContent = ui.edit ? "Done" : "Edit";
	button.setAttribute("aria-pressed", ui.edit ? "true" : "false");
	const addRoom = document.getElementById("add-room");
	addRoom.hidden = !ui.edit;
	addRoom.disabled = ui.busy;
}

/* Rebuild the page from ui. */
function render() {
	const root = document.getElementById("rooms");
	renderEditButton();
	if (ui.tree === null) {
		return;
	}
	renderHouseholdButton();
	if (ui.tree.rooms.length === 0) {
		root.replaceChildren(el("p", {text: "No rooms yet."}));
		return;
	}
	root.replaceChildren(...ui.tree.rooms.map(renderRoom));
}

/* The socket of the tree with this id, or null. */
function findSocket(id) {
	for (const room of ui.tree.rooms) {
		for (const sock of room.sockets) {
			if (sock.id === id) {
				return sock;
			}
		}
	}
	return null;
}

/* Put a socket that the server sent in place of the one in the tree. */
function replaceSocket(sock) {
	for (const room of ui.tree.rooms) {
		const at = room.sockets.findIndex((s) => s.id === sock.id);
		if (at >= 0) {
			room.sockets[at] = sock;
			return;
		}
	}
}

/* Show a switch that the server did: flash the row for a second. */
function flashRow(id) {
	ui.flash.add(id);
	setTimeout(() => ui.flash.delete(id), 1000);
}

/* Switch one socket. Only its own row is locked while the request
 * runs, and the outcome appears on that row. */
async function switchSocket(id, wanted) {
	if (ui.pending.has(id)) {
		return;
	}
	ui.notes.delete(id);
	ui.pending.set(id, wanted);
	render();
	const res = await api("POST", "/api/sockets/" + encodeURIComponent(id)
		+ "/switch", {state: wanted});
	if (res.ok && res.data && res.data.ok === true) {
		if (res.data.socket) {
			replaceSocket(res.data.socket);
		} else {
			await loadTree();
		}
		flashRow(id);
		if (res.data.recorded === false) {
			ui.notes.set(id, {kind: "info", text: "Sent, but not recorded"});
		}
	} else {
		const error = res.ok
			? {code: "http_error", message: "unexpected answer"}
			: res.error;
		ui.notes.set(id, {
			kind: "error", text: errorText(error, res.retryAfter)});
	}
	ui.pending.delete(id);
	render();
}

/*
 * Switch off the sockets of one room, or of the household when roomId is
 * null. The household asks first, with the number of sockets it will
 * switch off. The request is synchronous: it ends when the last socket
 * has been tried. The answer lists every socket, and each failed or
 * skipped socket gets a mark on its row.
 */
async function allOff(roomId) {
	const key = roomId === null ? HOUSEHOLD : roomId;
	if (ui.busyAllOff.has(key)) {
		return;
	}
	if (roomId === null && !window.confirm(
			"Switch off all " + countSockets() + " sockets?")) {
		return;
	}
	ui.busyAllOff.add(key);
	render();
	const path = roomId === null ? "/api/all-off"
		: "/api/rooms/" + encodeURIComponent(roomId) + "/all-off";
	const res = await api("POST", path, {});
	const data = res.data;
	if (res.ok && data && Array.isArray(data.results) && data.summary) {
		for (const result of data.results) {
			ui.notes.delete(result.socket_id);
			if (result.status === "ok") {
				const sock = findSocket(result.socket_id);
				if (sock && result.last_sent) {
					sock.last_sent = result.last_sent;
				}
				flashRow(result.socket_id);
				continue;
			}
			let text = errorText(result.error
				|| {code: "failed", message: "failed"});
			if (result.status === "skipped") {
				text = "skipped: " + text;
			}
			ui.notes.set(result.socket_id, {kind: "error", text: text});
		}
		const sum = data.summary;
		showBanner("All off: " + sum.ok + " ok, " + sum.error + " failed, "
			+ sum.skipped + " skipped", data.ok === true ? "info" : "error");
	} else {
		const error = res.ok
			? {code: "http_error", message: "unexpected answer"}
			: res.error;
		showBanner(errorText(error, res.retryAfter), "error");
	}
	ui.busyAllOff.delete(key);
	render();
}

/*
 * Load the limits of the socket types. The socket form takes its group
 * and number limits from them, so the page keeps no copy of its own.
 * Edit mode is off until they are here.
 */
async function loadMeta() {
	const res = await api("GET", "/api/meta");
	if (!res.ok || !res.data || typeof res.data.socket_types !== "object"
			|| res.data.socket_types === null) {
		const error = res.ok
			? {code: "http_error", message: "unexpected answer"}
			: res.error;
		showBanner("Editing is not available: "
			+ errorText(error, res.retryAfter), "error");
		ui.metaFailed = true;
		return;
	}
	ui.meta = res.data.socket_types;
	if (ui.metaFailed) {
		hideBanner();
		ui.metaFailed = false;
	}
	render();
}

/* Whether this browser can show the native dialogs of the edit forms. */
function dialogsSupported() {
	return typeof HTMLDialogElement === "function"
		&& typeof HTMLDialogElement.prototype.showModal === "function";
}

function toggleEdit() {
	if (!ui.edit && !dialogsSupported()) {
		showBanner("This browser cannot show the edit forms; switching "
			+ "still works.", "error");
		return;
	}
	ui.edit = !ui.edit;
	render();
}

/*
 * Run one management request and keep every management button disabled
 * while it runs, so the page never has two of them in flight. request
 * is a function that returns the answer of api(); it is also where a
 * successful change reloads the rooms, so the buttons stay disabled
 * until the page shows the result.
 */
async function manage(request) {
	setSaveButtons(true);
	ui.busy = true;
	render();
	try {
		return await request();
	} finally {
		ui.busy = false;
		setSaveButtons(false);
		render();
	}
}

/* The Save buttons of the two forms follow ui.busy too. */
function setSaveButtons(disabled) {
	document.getElementById("socket-save").disabled = disabled;
	document.getElementById("room-save").disabled = disabled;
}

/* Show text in a hidden error paragraph, or hide it for no text. */
function setMessage(node, text) {
	node.textContent = text || "";
	node.hidden = !text;
}

/*
 * The up and down buttons of the item at index among count items.
 * The first item cannot go up and the last one cannot go down. A tap
 * calls move(-1) or move(1).
 */
function arrowButtons(name, index, count, move) {
	const up = el("button", {
		type: "button", disabled: ui.busy || index === 0,
		"aria-label": "Move " + name + " up", text: "\u25B2"});
	up.addEventListener("click", () => move(-1));
	const down = el("button", {
		type: "button", disabled: ui.busy || index === count - 1,
		"aria-label": "Move " + name + " down", text: "\u25BC"});
	down.addEventListener("click", () => move(1));
	return [up, down];
}

/* Hide every field error and the dialog error of a dialog. */
function clearErrors(dialog) {
	for (const node of dialog.querySelectorAll(
			".field-error, .dialog-error")) {
		setMessage(node, "");
	}
}

/* Show the error of one field of the socket form. It returns false for
 * a field that the form does not have. */
function fieldError(field, message) {
	if (!Object.prototype.hasOwnProperty.call(FIELD_ERRORS, field)) {
		return false;
	}
	setMessage(document.getElementById(FIELD_ERRORS[field]), message);
	return true;
}

/* Show an error that belongs to no field, inside the dialog. */
function dialogError(dialog, text) {
	setMessage(dialog.querySelector(".dialog-error"), text);
}

/* Show why a save failed, next to the field when the server names one
 * and showField(field, message) knows the field. The dialog stays open
 * either way. */
function saveFailed(dialog, res, showField) {
	const error = res.ok
		? {code: "http_error", message: "unexpected answer"} : res.error;
	if (res.status === 422 && error.field
			&& showField(error.field, error.message)) {
		return;
	}
	dialogError(dialog, errorText(error, res.retryAfter));
}

/* The socket and room names of the sockets that a warning lists. */
function describeSockets(ids) {
	const names = [];
	for (const room of ui.tree.rooms) {
		for (const sock of room.sockets) {
			if (ids.includes(sock.id)) {
				names.push(sock.name + " (" + room.name + ")");
			}
		}
	}
	return names;
}

/* Tell the owner about a save that went through with a warning. The
 * tree is the reloaded one, so the names are current. */
function showWarnings(warnings) {
	for (const warning of warnings || []) {
		if (warning.code === "duplicate_address") {
			const names = describeSockets(warning.socket_ids || []);
			showBanner("Same address as: " + names.join(", "), "info");
		} else {
			showBanner(warning.message || warning.code, "info");
		}
	}
}

/* Set the options of a select from [value, label] pairs. */
function fillSelect(select, options, value) {
	select.replaceChildren(...options.map(([val, label]) =>
		el("option", {value: val, text: label})));
	select.value = value;
}

/* Allow only the groups and numbers that the chosen type has. */
function applyLimits() {
	const limits = ui.meta[document.getElementById("socket-type").value];
	const group = document.getElementById("socket-group");
	const number = document.getElementById("socket-number");
	if (!limits) {
		group.removeAttribute("max");
		number.removeAttribute("max");
		return;
	}
	group.min = 0;
	number.min = 0;
	group.max = limits.groups - 1;
	number.max = limits.sockets - 1;
}

/*
 * Open the form that adds a socket to the room roomId, or, with sock,
 * the form that edits that socket. The room field of an edit moves the
 * socket, so it starts on the room the socket is in.
 */
function openSocketDialog(roomId, sock) {
	const dialog = document.getElementById("socket-dialog");
	const types = Object.keys(ui.meta);
	if (sock && !types.includes(sock.type)) {
		types.push(sock.type);
	}
	ui.roomNotes.delete(roomId);
	if (sock) {
		ui.notes.delete(sock.id);
	}
	render();
	clearErrors(dialog);
	ui.editedSocket = sock || null;
	document.getElementById("socket-dialog-title").textContent
		= sock ? "Edit socket" : "Add socket";
	document.getElementById("socket-name").value = sock ? sock.name : "";
	fillSelect(document.getElementById("socket-type"),
		types.map((type) => [type, TYPE_LABELS[type] || type]),
		sock ? sock.type : types[0]);
	document.getElementById("socket-group").value = sock ? sock.group : "";
	document.getElementById("socket-number").value
		= sock ? sock.number : "";
	fillSelect(document.getElementById("socket-room"),
		ui.tree.rooms.map((room) => [room.id, room.name]), roomId);
	setMessage(document.getElementById("socket-address-note"), sock
		? "Changing type, group or number clears last sent." : "");
	applyLimits();
	dialog.showModal();
}

/* The fields of the socket form that differ from the socket being
 * edited, as the body of a PATCH. */
function changedFields(sock, fields) {
	const body = {};
	for (const key of Object.keys(fields)) {
		if (fields[key] !== sock[key]) {
			body[key] = fields[key];
		}
	}
	return body;
}

async function saveSocket(event) {
	event.preventDefault();
	if (ui.busy) {
		return;
	}
	const dialog = document.getElementById("socket-dialog");
	clearErrors(dialog);
	if (!document.getElementById("socket-form").reportValidity()) {
		return;
	}
	const group = document.getElementById("socket-group").valueAsNumber;
	const number = document.getElementById("socket-number").valueAsNumber;
	if (!Number.isInteger(group)) {
		fieldError("group", "Enter a whole number.");
		return;
	}
	if (!Number.isInteger(number)) {
		fieldError("number", "Enter a whole number.");
		return;
	}
	const roomId = document.getElementById("socket-room").value;
	let body = {
		name: document.getElementById("socket-name").value,
		type: document.getElementById("socket-type").value,
		group: group,
		number: number,
	};
	const sock = ui.editedSocket;
	let method = "POST";
	let path = "/api/rooms/" + encodeURIComponent(roomId) + "/sockets";
	if (sock) {
		/* Only what changed goes to the server. A new room moves the
		 * socket to the end of that room. */
		method = "PATCH";
		path = "/api/sockets/" + encodeURIComponent(sock.id);
		body = changedFields(sock, Object.assign({room_id: roomId}, body));
		if (Object.keys(body).length === 0) {
			dialog.close();
			return;
		}
	}
	const res = await manage(async () => {
		const answer = await api(method, path, body);
		/* A 404 says the socket is gone: show the rooms as they are. */
		if (answer.ok || (sock && answer.status === 404)) {
			await loadTree();
		}
		return answer;
	});
	if (!res.ok) {
		if (sock && res.status === 404) {
			dialogError(dialog, "This socket was deleted on another "
				+ "device.");
		} else {
			saveFailed(dialog, res, fieldError);
		}
		return;
	}
	dialog.close();
	showWarnings(res.data && res.data.warnings);
}

/*
 * Delete a socket after the owner confirmed it by name. A refusal
 * becomes the note of its row.
 */
async function deleteSocket(sock) {
	if (ui.busy) {
		return;
	}
	ui.notes.delete(sock.id);
	if (!window.confirm('Delete socket "' + sock.name + '"?')) {
		render();
		return;
	}
	const res = await manage(async () => {
		const answer = await api("DELETE", "/api/sockets/"
			+ encodeURIComponent(sock.id));
		if (answer.ok || answer.status === 404) {
			await loadTree();
		}
		return answer;
	});
	if (res.status === 404) {
		showBanner("This socket was deleted on another device.", "info");
	} else if (!res.ok) {
		ui.notes.set(sock.id, {
			kind: "error", text: errorText(res.error, res.retryAfter)});
		render();
	}
}

/*
 * Send the whole order of a list to the server. When another device
 * changed the list meanwhile, the server refuses the stale ids with
 * order_mismatch, or answers 404 for a room that is gone; either way
 * the page shows the rooms as they are and says so.
 */
async function reorder(path, ids) {
	const res = await manage(async () => {
		const answer = await api("PUT", path, {ids: ids});
		if (answer.ok || answer.status === 404
				|| (answer.error && answer.error.code === "order_mismatch")) {
			await loadTree();
		}
		return answer;
	});
	if (res.ok) {
		return;
	}
	if (res.status === 404 || res.error.code === "order_mismatch") {
		showBanner("The order was changed on another device; the page "
			+ "was reloaded.", "info");
	} else {
		showBanner(errorText(res.error, res.retryAfter), "error");
	}
}

/* The ids of items with the one at index swapped with its neighbour
 * delta places away, or null when there is no such neighbour. */
function swapped(items, index, delta) {
	const other = index + delta;
	if (other < 0 || other >= items.length) {
		return null;
	}
	const ids = items.map((item) => item.id);
	ids[index] = items[other].id;
	ids[other] = items[index].id;
	return ids;
}

/* Move the room at index one place up (delta -1) or down (1). */
async function moveRoom(index, delta) {
	const ids = ui.busy ? null : swapped(ui.tree.rooms, index, delta);
	if (ids !== null) {
		await reorder("/api/rooms/order", ids);
	}
}

/* Move the socket at index of room one place up or down. */
async function moveSocket(room, index, delta) {
	const ids = ui.busy ? null : swapped(room.sockets, index, delta);
	if (ids !== null) {
		await reorder("/api/rooms/" + encodeURIComponent(room.id)
			+ "/sockets/order", ids);
	}
}

/* Open the form that adds a room, or renames room. */
function openRoomDialog(room) {
	const dialog = document.getElementById("room-dialog");
	clearErrors(dialog);
	ui.editedRoom = room ? room.id : null;
	if (room) {
		ui.roomNotes.delete(room.id);
		render();
	}
	document.getElementById("room-dialog-title").textContent
		= room ? "Rename room" : "Add room";
	document.getElementById("room-name").value = room ? room.name : "";
	dialog.showModal();
}

/* Show the error of the name field of the room form. */
function roomFieldError(field, message) {
	if (field !== "name") {
		return false;
	}
	setMessage(document.getElementById("room-name-error"), message);
	return true;
}

async function saveRoom(event) {
	event.preventDefault();
	if (ui.busy) {
		return;
	}
	const dialog = document.getElementById("room-dialog");
	clearErrors(dialog);
	if (!document.getElementById("room-form").reportValidity()) {
		return;
	}
	const body = {name: document.getElementById("room-name").value};
	const id = ui.editedRoom;
	const res = await manage(async () => {
		const answer = id === null
			? await api("POST", "/api/rooms", body)
			: await api("PATCH", "/api/rooms/" + encodeURIComponent(id),
				body);
		/* A 404 says the room is gone: show the rooms as they are. */
		if (answer.ok || answer.status === 404) {
			await loadTree();
		}
		return answer;
	});
	if (!res.ok) {
		saveFailed(dialog, res, roomFieldError);
		return;
	}
	dialog.close();
}

/*
 * Delete an empty room after the owner confirmed it by name. A room
 * that still holds sockets is refused by the server, and the reason
 * stays on its card until the next action on the card.
 */
async function deleteRoom(room) {
	if (ui.busy) {
		return;
	}
	ui.roomNotes.delete(room.id);
	if (!window.confirm('Delete room "' + room.name + '"?')) {
		render();
		return;
	}
	const res = await manage(async () => {
		const answer = await api("DELETE", "/api/rooms/"
			+ encodeURIComponent(room.id));
		if (answer.ok || answer.status === 404) {
			await loadTree();
		}
		return answer;
	});
	if (res.status === 404) {
		showBanner("This room was deleted on another device.", "info");
	} else if (!res.ok) {
		ui.roomNotes.set(room.id, errorText(res.error, res.retryAfter));
		render();
	}
}

document.addEventListener("DOMContentLoaded", () => {
	document.getElementById("all-off").addEventListener(
		"click", () => allOff(null));
	document.getElementById("edit-toggle").addEventListener(
		"click", toggleEdit);
	document.getElementById("socket-form").addEventListener(
		"submit", saveSocket);
	document.getElementById("socket-type").addEventListener(
		"change", applyLimits);
	document.getElementById("socket-cancel").addEventListener(
		"click", () => document.getElementById("socket-dialog").close());
	document.getElementById("add-room").addEventListener(
		"click", () => openRoomDialog(null));
	document.getElementById("room-form").addEventListener(
		"submit", saveRoom);
	document.getElementById("room-cancel").addEventListener(
		"click", () => document.getElementById("room-dialog").close());
	loadTree();
	loadMeta();
});

/*
 * A page that stays open on one phone shows the rooms as they were when
 * it was loaded. Load them again when the page is shown again, for
 * example after switching back to the browser. There is no timer:
 * render() reads ui.pending and ui.notes, so the rows in flight stay
 * disabled and the messages under the rows stay.
 */
document.addEventListener("visibilitychange", () => {
	if (document.visibilityState === "visible") {
		loadTree();
		if (ui.meta === null) {
			loadMeta();
		}
	}
});

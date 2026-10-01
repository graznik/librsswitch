/*
 * app.js - the script of rsswitch-web: shows the rooms and sockets of
 * the JSON API and switches a socket when On or Off is tapped.
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

/*
 * What the page remembers besides the rooms. render() reads all of it,
 * so a rebuilt page keeps the rows that are in flight and the messages
 * under the rows.
 *   tree     the answer of GET /api/rooms, or null before the first one
 *   pending  socket id -> "on" or "off" for each switch request in flight
 *   notes    socket id -> {kind: "error" | "info", text}
 *   flash    ids of the rows that flash after a successful switch
 *   loadSeq  number of the newest request for the rooms
 */
const ui = {
	tree: null,
	pending: new Map(),
	notes: new Map(),
	flash: new Set(),
	loadSeq: 0,
	loadFailed: false,
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
	banner.textContent = text;
	banner.className = kind === "error" ? "banner banner-error" : "banner";
	banner.hidden = false;
}

function hideBanner() {
	const banner = document.getElementById("banner");
	banner.textContent = "";
	banner.hidden = true;
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
		ui.loadFailed = true;
		return;
	}
	if (ui.loadFailed) {
		hideBanner();
		ui.loadFailed = false;
	}
	ui.tree = res.data;
	render();
}

function renderSocket(sock) {
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
			el("span", {class: "socket-name", text: sock.name}), last),
		el("div", {class: "switch"}, ...buttons));
	if (note) {
		row.append(el("p", {
			class: "note note-" + note.kind, text: note.text}));
	}
	return row;
}

function renderRoom(room) {
	const sockets = room.sockets.map(renderSocket);
	if (sockets.length === 0) {
		sockets.push(el("p", {class: "hint", text: "No sockets yet."}));
	}
	return el("section", {class: "room"},
		el("div", {class: "room-head"}, el("h2", {text: room.name})),
		...sockets);
}

/* Rebuild the page from ui. */
function render() {
	const root = document.getElementById("rooms");
	if (ui.tree === null) {
		return;
	}
	if (ui.tree.rooms.length === 0) {
		root.replaceChildren(el("p", {text: "No rooms yet."}));
		return;
	}
	root.replaceChildren(...ui.tree.rooms.map(renderRoom));
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
		ui.flash.add(id);
		setTimeout(() => ui.flash.delete(id), 1000);
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

document.addEventListener("DOMContentLoaded", loadTree);

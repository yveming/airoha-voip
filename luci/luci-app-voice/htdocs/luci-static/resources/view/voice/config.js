// SPDX-License-Identifier: Apache-2.0

'use strict';
'require form';
'require fs';
'require uci';
'require ui';
'require view';

var STYLESHEET = 'view/voice/voice.css';

/* Fully qualified name of the protocol list: <config>.<section>.<option>. */
var PROTOCOL_OPTION = 'voice.config.protocol';

/*
 * The board carries a single FXS port, so there is no port to pick and no
 * line card.  The PON views follow luci-theme-argon: cards, form rows and
 * buttons come from the theme; only the small helper stylesheet is ours.
 */
function ensureStylesheet() {
	var href;

	if (document.querySelector('link[data-pon-stylesheet]'))
		return;

	href = (typeof L.resource == 'function') ? L.resource(STYLESHEET) : null;

	if (!href)
		href = ((L.env && L.env.base_url) || '/luci-static/resources') + '/' + STYLESHEET;

	document.head.appendChild(E('link', {
		'rel': 'stylesheet',
		'href': href,
		'data-pon-stylesheet': ''
	}));
}

function validateFlashHookRange(sectionId, value) {
	var minimum = parseInt(this.section.formvalue(sectionId, 'flashhook_min'), 10);

	/* An empty minimum is reported by its own required check; only compare
	   the two values when both are numbers. */
	if (isNaN(minimum) || parseInt(value, 10) > minimum)
		return true;

	return _('The maximum flash hook interval must be greater than the minimum one.');
}

/*
 * LuCI combines the keys of a single depends({ ... }) object with "and" while
 * treating repeated depends() calls as alternatives ("or"), so the protocol
 * switch and any nested constraint have to be spelled out as one object per
 * protocol value. A key containing a dot is resolved as cbid.<key>, which is
 * how an option living in another section is reached - here the protocol list
 * of the "config" section.
 */
function dependsOnProtocols(o, protocols, extra) {
	protocols.forEach(function(protocol) {
		var dep = {};

		dep[PROTOCOL_OPTION] = protocol;

		for (var name in extra || {})
			dep[name] = extra[name];

		o.depends(dep);
	});

	return o;
}

/* The SIP block serves both the softswitch and the IMS flavour. */
function forSip(o, extra) {
	return dependsOnProtocols(o, [ 'sip', 'ims_sip' ], extra);
}

function validateIfname(sectionId, value) {
	if (!/^[A-Za-z0-9_.:-]{1,15}$/.test(value || ''))
		return _('Use a Linux network device name containing at most 15 characters.');

	return true;
}

/*
 * voice-apply refuses to share a VLAN with the internet or the IPTV page: two
 * services on one 8021q subinterface would end up in one zone and defeat the
 * isolation the voice zone exists for.
 */
function validateVoiceVlan(sectionId, value) {
	var vid = String(value || '');
	var uplink = (this.section && this.section.formvalue(sectionId, 'uplink')) ||
		uci.get('voice', 'network', 'uplink') || 'pon0';
	var devs, service, mcast, i;
	var managed = uci.get('voice', 'network', 'name') || 'voice';
	var legacy = uci.get('network', 'voip', 'device');

	if (!vid)
		return true;

	devs = uci.sections('network', 'device') || [];
	for (i = 0; i < devs.length; i++) {
		/*
		 * The page itself creates network.luci_voice_vlan (name=voice)
		 * and voice-apply deletes/recreates it on every apply.  Do not
		 * report that managed device as a foreign VLAN, and allow the
		 * legacy voip/pon0.<vid> device while it is being migrated.
		 */
		if (devs[i]['.name'] === 'luci_voice_vlan')
			continue;
		if (devs[i].name === managed)
			continue;
		if (legacy && devs[i].name === legacy)
			continue;
		if (devs[i].name === 'voip' || devs[i].name === uplink + '.' + vid)
			continue;
		if (devs[i].type === '8021q' && devs[i].ifname === uplink &&
		    String(devs[i].vid) === vid)
			return _('This VLAN is already used by %s; pick a different one.')
				.format(devs[i].name || ('VLAN ' + vid));
	}

	service = uci.get('iptv', 'config', 'service_vid');
	mcast = uci.get('iptv', 'config', 'mcast_vid');
	if (vid === String(service) || vid === String(mcast))
		return _('This VLAN carries IPTV; pick a different one for voice.');

	return true;
}

/*
 * LuCI renders the SIP section container even when its options are gated on
 * the protocol. Keep the always-present note in the card and hide the whole
 * card when the selected protocol is not SIP/IMS SIP.
 */
function sectionCard(node, name) {
	/*
	 * A NamedSection whose name equals its UCI type (the sip
	 * sections below) makes LuCI stamp the same id, cbi-voice-<name>, on both
	 * the outer .cbi-section card and the inner .cbi-section-node. Pin the
	 * selector to the card so the heading collapses with the body; fall back
	 * to the bare id if a future LuCI no longer carries the class.
	 */
	var card = node.querySelector('#cbi-voice-' + name + '.cbi-section')
		|| node.querySelector('#cbi-voice-' + name);

	return card || null;
}

function toggleProtocolSections(node, protocolOption) {
	var sip = sectionCard(node, 'sip');
	var frame = node.querySelector('#cbi-voice-config-protocol');

	function update() {
		var protocol = protocolOption ? protocolOption.formvalue('config') : null;

		if (sip && sip.classList)
			sip.classList[(protocol === 'sip' || protocol === 'ims_sip') ? 'remove' : 'add']('hidden');
	}

	if (protocolOption)
		protocolOption.onchange = update;

	if (frame) {
		frame.addEventListener('change', update);
		frame.addEventListener('input', update);
	}

	update();
}

function serverOption(s, tab, name, title, descr) {
	var o = s.taboption(tab, form.Value, name, title, descr);

	o.datatype = 'or(hostname,ipaddr)';
	o.rmempty = true;

	return forSip(o);
}

function portOption(s, tab, name, title) {
	var o = s.taboption(tab, form.Value, name, title);

	o.datatype = 'range(1,65534)';
	o.placeholder = '5060';
	o.rmempty = false;

	return forSip(o);
}

function transportOption(s, tab, name, title) {
	var o = s.taboption(tab, form.ListValue, name, title);

	o.default = 'udp';
	o.value('udp', 'UDP');
	o.value('tcp', 'TCP');
	o.value('tls', 'TLS');
	o.rmempty = false;

	return forSip(o);
}

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('voice'),
			uci.load('network'),
			uci.load('iptv').catch(function() { return null; })
		]);
	},

	handleSaveApply: function(ev, mode) {
		return this.super('handleSaveApply', [ev, mode]).then(function() {
			return fs.exec('/usr/libexec/voice-apply', [ 'apply' ]);
		}).then(function(res) {
			if (res && res.code != 0)
				throw new Error(res.stderr || res.stdout || _('voice-apply failed'));

			ui.addNotification(null, E('p', _('Voice configuration applied.')), 'info');
		}).catch(function(err) {
			ui.addNotification(null, E('p', _('Voice configuration failed: %s').format(err.message)), 'danger');
		});
	},

	render: function() {
		var m, s, o, protocolOption;

		ensureStylesheet();

		m = new form.Map('voice', null, _('Configure the voice service network, SIP account, digit map and codecs.'));
		m.readonly = !L.hasViewPermission();

		/* ---------------------------------------------------------------- *
		 * Voice network                                                    *
		 * ---------------------------------------------------------------- */

		/*
	 * The voice VLAN is a single L3 link of the ONU (voice, own zone).  The
	 * section is unsigned and lives before the profile so the topology is read
	 * before the signalling that rides on it.
	 */
		s = m.section(form.NamedSection, 'network', 'network', _('Voice service network'),
			_('The voice VLAN is terminated locally (route mode).'));
		s.anonymous = true;
		s.addremove = false;

		o = s.option(form.Flag, 'enabled', _('Enable'));
		o.default = '0';
		o.rmempty = false;

		o = s.option(form.Value, 'uplink', _('Uplink device'),
			_('Network device carrying the operator VLANs, normally pon0.'));
		o.default = 'pon0';
		o.rmempty = false;
		o.validate = function(sectionId, value) {
			if (!/^[A-Za-z0-9_.:-]{1,15}$/.test(value || ''))
				return _('Use a Linux network device name containing at most 15 characters.');
			return true;
		};
		o.depends('enabled', '1');

		o = s.option(form.Value, 'name', _('Device name'),
			_('Linux name of the voice VLAN device. Default: voice.'));
		o.default = 'voice';
		o.rmempty = false;
		o.validate = validateIfname;
		o.depends('enabled', '1');

		o = s.option(form.Value, 'vlan_id', _('VLAN ID'),
			_('VLAN carrying the voice service.'));
		o.datatype = 'range(1,4094)';
		o.placeholder = '例如 45';
		o.rmempty = false;
		o.validate = validateVoiceVlan;
		o.depends('enabled', '1');

		o = s.option(form.Value, 'macaddr', _('MAC address'),
			_('Optional. Override the MAC address of the voice interface.'));
		o.datatype = 'macaddr';
		o.rmempty = true;
		o.depends('enabled', '1');

		o = s.option(form.ListValue, 'proto', _('IP assignment'),
			_('How the voice interface obtains its address.'));
		o.default = 'dhcp';
		o.rmempty = false;
		o.value('dhcp', _('DHCP'));
		o.value('static', _('Static'));
		o.depends('enabled', '1');

		o = s.option(form.Value, 'ipaddr', _('IP address'),
			_('Address of the voice interface on the voice VLAN.'));
		o.datatype = 'ip4addr';
		o.placeholder = '10.0.0.2';
		o.rmempty = false;
		o.depends({ enabled: '1', proto: 'static' });

		o = s.option(form.Value, 'netmask', _('Subnet mask'));
		o.datatype = 'ip4addr';
		o.placeholder = '255.255.255.0';
		o.rmempty = false;
		o.depends({ enabled: '1', proto: 'static' });

		o = s.option(form.Value, 'gateway', _('Gateway'),
			_('Gateway of the voice VLAN, used for the table-101 default route.'));
		o.datatype = 'ip4addr';
		o.placeholder = '10.0.0.1';
		o.rmempty = true;
		o.depends({ enabled: '1', proto: 'static' });

		o = s.option(form.Value, 'dns', _('DNS servers'),
			_('Comma-separated DNS servers used by the voice service.'));
		o.datatype = 'list(ip4addr)';
		o.placeholder = '10.0.0.1';
		o.rmempty = true;
		o.depends({ enabled: '1', proto: 'static' });

		/* ---------------------------------------------------------------- *
		 * Voice profile                                                    *
		 * ---------------------------------------------------------------- */

		s = m.section(form.NamedSection, 'config', 'voice', _('Voice service'),
			_('Signalling protocol, DTMF transfer, tone timers and registration account of the FXS port.'));
		s.anonymous = true;
		s.addremove = false;

		o = s.option(form.Flag, 'enabled', _('Enable'));
		o.default = '1';
		o.rmempty = false;

		o = s.option(form.ListValue, 'test_mode', _('Test mode'),
			_('Echo returns the audio to the phone; SIP modes send the call to the provider.'));
		o.default = 'echo';
		o.value('echo', _('Echo test'));
		o.value('sip_read', _('SIP: read digits from the phone'));
		o.value('sip_fixed', _('SIP: dial a fixed number'));
		o.value('sip_digitmap', _('SIP: use the dial plan'));
		o.rmempty = false;

		o = s.option(form.Value, 'fixed_number', _('Fixed destination'));
		o.depends('test_mode', 'sip_fixed');
		o.rmempty = true;

		o = s.option(form.Value, 'line_name', _('Channel line name'));
		o.default = 'line1';
		o.rmempty = false;
		o.description = _('Changing this requires an Asterisk restart.');

		o = s.option(form.Value, 'dial_timeout', _('Dial timeout (s)'));
		o.datatype = 'uinteger';
		o.default = '60';
		o.rmempty = false;

		o = s.option(form.Flag, 'inbound', _('Inbound SIP calls ring the FXS phone'));
		o.default = '1';
		o.rmempty = false;

		protocolOption = o = s.option(form.ListValue, 'protocol', _('Voice protocol'),
			_('The signalling protocol spoken with the softswitch.'));
		o.default = 'ims_sip';
		o.value('sip', _('Softswitch SIP'));
		o.value('ims_sip', _('IMS SIP'));
		o.rmempty = false;

		o = s.option(form.ListValue, 'dtmf_method', _('DTMF transfer mode'));
		o.default = 'RFC2833';
		o.value('InBand', _('Transparent (in-band)'));
		o.value('RFC2833', _('RFC 2833'));
		o.value('SIPInfo', _('SIP INFO'));
		o.rmempty = false;

		o = s.option(form.Value, 'dtmf_payload', _('Payload type value'),
			_('Stored for reference; PJSIP negotiates the telephone-event payload from SDP.'));
		o.datatype = 'range(96,127)';
		o.placeholder = '97';
		o.rmempty = false;
		o.depends('dtmf_method', 'RFC2833');

		o = s.option(form.ListValue, 'clip_mode', _('Calling line identification'),
			_('How the calling number is presented on the FXS port.'));
		o.default = 'FSK';
		o.value('FSK', _('FSK'));
		o.rmempty = false;

		o = s.option(form.Value, 'flashhook_min', _('Flash hook minimum (ms)'),
			_('Shortest hook flash still recognised as a flash.'));
		o.datatype = 'uinteger';
		o.placeholder = '90';
		o.rmempty = false;

		o = s.option(form.Value, 'flashhook_max', _('Flash hook maximum (ms)'),
			_('Longest hook flash still recognised as a flash.'));
		o.datatype = 'uinteger';
		o.placeholder = '500';
		o.rmempty = false;
		o.validate = validateFlashHookRange;

		o = s.option(form.Value, 'hanging_reminder_tone_timer', _('Hanging reminder tone timer (s)'),
			_('How long the on-hook reminder tone plays before the call is released.'));
		o.datatype = 'range(1,3600)';
		o.placeholder = '40';
		o.rmempty = false;

		o = s.option(form.Value, 'busy_tone_timer', _('Busy tone timer (s)'),
			_('How long the busy tone plays before the call is released.'));
		o.datatype = 'range(1,3600)';
		o.placeholder = '40';
		o.rmempty = false;

		o = s.option(form.Value, 'no_answer_timer', _('No answer timer (s)'),
			_('How long an unanswered call rings before it is released.'));
		o.datatype = 'range(1,3600)';
		o.placeholder = '60';
		o.rmempty = false;

		o = s.option(form.ListValue, 'codec_prefer_rule', _('Codec negotiation rule'),
			_('Stored for reference; the current backend uses the codec priority order.'));
		o.default = 'RemoteFirst';
		o.value('RemoteFirst', _('Remote first'));
		o.value('LocalFirst', _('Local first'));
		o.rmempty = false;

		o = s.option(form.ListValue, 'fax_mode', _('Fax codec'),
			_('G.711 passthrough is supported; T.38 is stored but not implemented.'));
		o.default = 'g711';
		o.value('t38', _('T.38'));
		o.value('g711', _('G.711'));
		o.rmempty = false;

		o = s.option(form.ListValue, 'fax_negotiation', _('Fax negotiation mode'),
			_('How the fax call is negotiated when the G.711 codec is used.'));
		o.default = 'all';
		o.value('all', _('T.30 full control'));
		o.value('other', _('T.30 auto negotiation'));
		o.rmempty = false;
		o.depends('fax_mode', 'g711');

		o = s.option(form.Flag, 'time_sync', _('Synchronize the phone clock'),
			_('Send the gateway time to the connected phone.'));
		o.default = '0';
		o.rmempty = false;

		o = s.option(form.Value, 'transmit_gain', _('Transmit gain (dB)'),
			_('Gain applied to the audio sent towards the network.'));
		o.datatype = 'range(-14,6)';
		o.placeholder = '0';
		o.rmempty = false;

		o = s.option(form.Value, 'receive_gain', _('Receive gain (dB)'),
			_('Gain applied to the audio received from the network.'));
		o.datatype = 'range(-14,6)';
		o.placeholder = '0';
		o.rmempty = false;

		o = s.option(form.Flag, 'echo_cancellation', _('Enable echo cancellation'),
			_('OSLEC line echo canceller in the AN7581 PCM path (16 ms tail). Disable if it makes audio worse.'));
		o.default = '1';
		o.rmempty = false;

		/* ---------------------------------------------------------------- *
		 * SIP                                                              *
		 * ---------------------------------------------------------------- */

		s = m.section(form.NamedSection, 'sip', 'sip', _('SIP'),
			_('Session Initiation Protocol settings used with the softswitch and IMS SIP voice protocols.'));
		s.anonymous = true;
		s.addremove = false;
		s.tab('auth', _('Authentication'));
		s.tab('server', _('Servers'));
		s.tab('standby', _('Standby servers'));
		s.tab('advanced', _('Advanced settings'));
		s.tab('heartbeat', _('Heartbeat'));

	/*
	 * Every SIP option is gated on the SIP protocols.  The always-present note
	 * keeps the tabbed card non-empty when the protocol is changed.
	 */
	s.taboption('server', form.DummyValue, '_protocol', _('SIP voice protocol'),
		_('Softswitch and IMS SIP signalling parameters.'));

		o = forSip(s.taboption('auth', form.Value, 'auth_username',
			_('Authentication user name'),
			_('User name the FXS port authenticates with.')));
		o.rmempty = true;

		o = forSip(s.taboption('auth', form.Value, 'auth_password',
			_('Authentication password')));
		o.password = true;
		o.rmempty = true;

		o = forSip(s.taboption('auth', form.Value, 'phone_number',
			_('Phone number'),
			_('Number presented to the called party.')));
		o.rmempty = true;

		o = forSip(s.taboption('server', form.Flag, 'register', _('Register with the SIP server')));
		o.default = '1';
		o.rmempty = false;

		serverOption(s, 'server', 'proxy_server', _('Proxy server'),
			_('Address of the SIP proxy, an IP address or a domain name.'));
		portOption(s, 'server', 'proxy_port', _('Proxy server port'));
		transportOption(s, 'server', 'proxy_transport', _('Proxy server transport'));

		serverOption(s, 'server', 'register_server', _('Registrar server'),
			_('Address of the SIP registrar, an IP address or a domain name.'));
		portOption(s, 'server', 'register_port', _('Registrar server port'));
		transportOption(s, 'server', 'register_transport', _('Registrar server transport'));

		serverOption(s, 'server', 'outbound_server', _('Outbound proxy server'),
			_('Address of the outbound proxy. Leave empty to send requests straight to the proxy server.'));
		portOption(s, 'server', 'outbound_port', _('Outbound proxy port'));

		serverOption(s, 'standby', 'standby_proxy_server', _('Standby proxy server'),
			_('Address used when the SIP proxy cannot be reached.'));
		portOption(s, 'standby', 'standby_proxy_port', _('Standby proxy server port'));
		transportOption(s, 'standby', 'standby_proxy_transport', _('Standby proxy server transport'));

		serverOption(s, 'standby', 'standby_register_server', _('Standby registrar server'),
			_('Address used when the SIP registrar cannot be reached.'));
		portOption(s, 'standby', 'standby_register_port', _('Standby registrar server port'));
		transportOption(s, 'standby', 'standby_register_transport', _('Standby registrar server transport'));

		serverOption(s, 'standby', 'standby_outbound_server', _('Standby outbound proxy server'),
			_('Address of the outbound proxy used when the primary one cannot be reached.'));
		portOption(s, 'standby', 'standby_outbound_port', _('Standby outbound proxy port'));

		o = forSip(s.taboption('advanced', form.Value, 'signalling_dscp', _('Signalling DSCP'),
			_('DSCP mark of the SIP signalling packets.')));
		o.datatype = 'range(0,63)';
		o.placeholder = '0';
		o.rmempty = false;

		o = forSip(s.taboption('advanced', form.Value, 'media_dscp', _('Media DSCP'),
			_('DSCP mark of the voice packets.')));
		o.datatype = 'range(0,63)';
		o.placeholder = '0';
		o.rmempty = false;

		o = forSip(s.taboption('advanced', form.Value, 'register_period', _('Registration period (s)'),
			_('How often the gateway registers again.')));
		o.datatype = 'uinteger';
		o.placeholder = '3600';
		o.rmempty = false;

		o = forSip(s.taboption('advanced', form.Value, 'register_retry_period', _('Registration retry period (s)'),
			_('How soon a failed registration is retried.')));
		o.datatype = 'uinteger';
		o.placeholder = '60';
		o.rmempty = false;

		o = forSip(s.taboption('advanced', form.Value, 'session_update_period', _('Session update period (min)'),
			_('How often an established call refreshes its session.')));
		o.datatype = 'uinteger';
		o.placeholder = '30';
		o.rmempty = false;

		o = forSip(s.taboption('advanced', form.Value, 'min_session_update_period', _('Minimum session update period (min)'),
			_('Shortest session refresh interval accepted from the server.')));
		o.datatype = 'uinteger';
		o.placeholder = '1';
		o.rmempty = false;

		o = forSip(s.taboption('heartbeat', form.Flag, 'heartbeat_enabled', _('Enable heartbeat'),
			_('Detect whether the server is still reachable while no call is up.')));
		o.default = '0';
		o.rmempty = false;

	o = forSip(s.taboption('heartbeat', form.Value, 'heartbeat_period', _('Heartbeat period (s)')), { heartbeat_enabled: '1' });
	o.datatype = 'uinteger';
	o.placeholder = '60';
	o.rmempty = false;

	o = forSip(s.taboption('heartbeat', form.Value, 'heartbeat_timeout_count', _('Heartbeat timeout count'),
		_('Heartbeat timeouts before the registration is refreshed.')), { heartbeat_enabled: '1' });
	o.datatype = 'uinteger';
	o.placeholder = '3';
	o.rmempty = false;

	o = forSip(s.taboption('heartbeat', form.ListValue, 'heartbeat_mode', _('Heartbeat mode')), { heartbeat_enabled: '1' });
	o.default = 'active';
	o.value('active', _('Active'));
	o.value('passive', _('Passive'));
	o.rmempty = false;

		/* ---------------------------------------------------------------- *
		 * Digit map                                                        *
		 * ---------------------------------------------------------------- */

		s = m.section(form.NamedSection, 'digitmap', 'digitmap', _('Digit map'),
			_('Number patterns that end dialling and the timers guarding them.'));
		s.anonymous = true;
		s.addremove = false;

		o = s.option(form.Flag, 'enabled', _('Enable dial plan'));
		o.default = '1';
		o.rmempty = false;

		o = s.option(form.ListValue, 'match_mode', _('Matching mode'),
			_('Whether dialling ends at the shortest or the longest matching pattern.'));
		o.default = 'max';
		o.value('max', _('Longest match'));
		o.value('min', _('Shortest match'));
		o.rmempty = false;

		o = s.option(form.Value, 'offhook_timer', _('Off-hook without dialling timer (s)'),
			_('How long the port waits after going off-hook before it plays the congestion tone.'));
		o.datatype = 'range(10,20)';
		o.placeholder = '15';
		o.rmempty = false;

		o = s.option(form.Value, 'short_timer', _('Inter-digit short timer (s)'),
			_('Used while the dialled digits can still start more than one pattern.'));
		o.datatype = 'range(1,60)';
		o.placeholder = '5';
		o.rmempty = false;

		o = s.option(form.Value, 'long_timer', _('Inter-digit long timer (s)'),
			_('Used once the dialled digits can only complete a long number.'));
		o.datatype = 'range(1,60)';
		o.placeholder = '16';
		o.rmempty = false;

		o = s.option(form.TextValue, 'digit_map', _('Dial plan patterns'),
			_('Dialled number patterns separated by a vertical bar. Leave empty to let the softswitch decide.'));
		o.rows = 4;
		o.monospace = true;
		o.rmempty = true;
		o.placeholder = '01[34578]xxxxxxxxx|1[34578]xxxxxxxxx|10086|11[0249]|x.T';

		/* ---------------------------------------------------------------- *
		 * Codecs                                                           *
		 * ---------------------------------------------------------------- */

		/*
		 * One FXS port means one codec list: a table of codecs is enough,
		 * there is no port column to pick from any more.
		 */
		s = m.section(form.TableSection, 'codec', _('Codecs'),
			_('Codecs offered by the FXS port, ordered by priority.'));
		s.anonymous = true;
		s.addremove = true;

		o = s.option(form.ListValue, 'codec', _('Codec'));
		o.value('G.711ALaw', 'G.711 A-law');
		o.value('G.711MuLaw', 'G.711 u-law');
		o.value('G.722', 'G.722');
		o.value('G.729', 'G.729');
		o.rmempty = false;

		o = s.option(form.ListValue, 'packetization', _('Packetization period (ms)'),
			_('Audio milliseconds carried by a single RTP packet.'));
		o.default = '20';
		o.value('10', '10');
		o.value('20', '20');
		o.value('30', '30');
		o.rmempty = false;

		o = s.option(form.Value, 'priority', _('Priority'),
			_('Lower values are offered first.'));
		o.datatype = 'uinteger';
		o.placeholder = '1';
		o.rmempty = false;

		return m.render().then(function(node) {
			toggleProtocolSections(node, protocolOption);

			return node;
		});
	}
});

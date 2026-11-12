// SPDX-License-Identifier: Apache-2.0

'use strict';
'require view';
'require fs';
'require ui';

function parseStatus(text) {
	var out = {};

	(text || '').split('\n').forEach(function(line) {
		var i = line.indexOf('=');

		if (i > 0)
			out[line.substring(0, i)] = line.substring(i + 1);
	});

	return out;
}

function row(label, value) {
	return E('tr', {}, [
		E('td', { 'style': 'width:35%;color:#666' }, [ label ]),
		E('td', {}, [ value == null || value === '' ? '—' : value ])
	]);
}

function section(title, rows) {
	return E('div', { 'class': 'cbi-section' }, [
		E('h3', {}, [ title ]),
		E('table', { 'class': 'table' }, [ E('tbody', {}, rows) ])
	]);
}

function enabled(value) {
	return value === '1' ? _('Enabled') : _('Disabled');
}

function protocolName(value) {
	switch (value) {
	case 'sip': return _('SIP');
	case 'ims_sip': return _('IMS SIP');
	default: return value || '—';
	}
}

function testModeName(value) {
	switch (value) {
	case 'echo': return _('Echo test');
	case 'sip_digitmap': return _('SIP dial plan');
	case 'sip_fixed': return _('SIP fixed number');
	case 'sip_read': return _('SIP read digits');
	default: return value || '—';
	}
}

function networkStateName(value) {
	switch (value) {
	case 'disabled': return _('Disabled');
	case 'missing': return _('Device missing');
	case 'no-carrier': return _('No carrier');
	case 'no-ip': return _('Link up, no IP');
	case 'up': return _('Up');
	default: return value || '—';
	}
}

function registrationName(value) {
	switch (value) {
	case 'Registered': return _('Registered');
	case 'Unregistered': return _('Unregistered');
	case 'Rejected': return _('Rejected');
	case 'Stopped': return _('Stopped');
	case 'Unreachable': return _('Unreachable');
	case 'disabled': return _('Registration disabled');
	case 'not-loaded': return _('Endpoint not loaded');
	case 'asterisk-down': return _('Asterisk stopped');
	case 'not-configured': return _('Not configured');
	case 'unknown': return _('Unknown');
	default: return value || '—';
	}
}

function hookName(value) {
	switch (value) {
	case 'onhook': return _('On-hook');
	case 'offhook': return _('Off-hook');
	default: return value || '—';
	}
}

function fxsStateName(value) {
	switch (value) {
	case '0x23': return _('Codec path ready');
	case '0x00': return _('Idle');
	default: return value || '—';
	}
}

function sipNote(d) {
	if (d.asterisk_running !== '1')
		return _('Asterisk is not running.');
	if (d.sip_configured !== '1')
		return _('No SIP account configured; fill it in Configuration.');
	if (d.sip_endpoint !== '1')
		return _('PJSIP endpoint is not loaded; check the Asterisk log.');
	if (d.sip_register === '1' && d.sip_registration !== 'Registered')
		return _('Registration is not active; check the server and credentials.');
	return null;
}

function actionButtons(view) {
	return E('div', { 'class': 'cbi-page-actions' }, [
		E('button', {
			'class': 'cbi-button cbi-button-action',
			'click': ui.createHandlerFn(view, 'handleRestart')
		}, [ _('Restart Asterisk') ]),
		' ',
		E('button', {
			'class': 'cbi-button',
			'click': function() { window.location.reload(); }
		}, [ _('Refresh') ])
	]);
}

return view.extend({
	load: function() {
		return fs.exec('/usr/libexec/voip-apply', [ 'status' ]).catch(function() {
			return { code: 1, stdout: '', stderr: '' };
		});
	},

	handleRestart: function() {
		return fs.exec('/usr/libexec/voip-apply', [ 'restart' ]).then(function(res) {
			if (res.code != 0)
				throw new Error(res.stderr || res.stdout || _('restart failed'));

			ui.addNotification(null, E('p', _('Asterisk restarted. Reloading...')), 'info');
			window.setTimeout(function() { window.location.reload(); }, 1500);
		}).catch(function(err) {
			ui.addNotification(null, E('p', _('Asterisk restart failed: %s').format(err.message)), 'danger');
		});
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null,

	render: function(res) {
		var text = (res && (res.stdout || res.stderr)) || '';
		var d = parseStatus(text);

		if (!Object.keys(d).length) {
			return E('div', { 'class': 'cbi-map' }, [
				E('div', { 'class': 'cbi-map-descr' },
					_('Show the runtime state of the voice service network, SIP registration and FXS line.')),
				E('div', { 'class': 'cbi-section' }, [
					E('pre', { 'style': 'white-space:pre-wrap' },
						[ text || _('No status available.') ])
				]),
				actionButtons(this)
			]);
		}

		var serviceRows = [
			row(_('Status'), enabled(d.service_enabled)),
			row(_('Protocol'), protocolName(d.service_protocol)),
			row(_('Test mode'), testModeName(d.service_test_mode)),
			row(_('Asterisk'), d.asterisk_running === '1' ? _('Running') : _('Stopped')),
			row(_('Active calls'), String(d.active_calls || '0'))
		];

		if (d.service_enabled !== '1')
			serviceRows.push(row(_('Note'), _('Voice service is disabled.')));

		var rule = null;
		if (d.network_enabled === '1' && d.network_ip)
			rule = d.network_rule === '1' ? _('Configured') : _('Missing');

		var networkRows = [
			row(_('Status'), enabled(d.network_enabled)),
			row(_('VLAN'), d.network_vlan || '—'),
			row(_('Device'), d.network_device || '—'),
			row(_('Interface'), d.network_interface || '—'),
			row(_('Link'), networkStateName(d.network_state)),
			row(_('IPv4 address'), d.network_ip || '—'),
			row(_('Gateway'), d.network_gateway || '—'),
			row(_('Policy table'), d.network_table || '—'),
			row(_('MAC'), d.network_mac || '—'),
			row(_('Policy rule'), rule || '—')
		];

		var sipRows = [
			row(_('SIP account'), d.sip_configured === '1' ? _('Configured') : _('Not configured')),
			row(_('Registrar'), d.sip_registrar || '—'),
			row(_('Proxy'), d.sip_proxy || '—'),
			row(_('Transport'), d.sip_transport || '—'),
			row(_('Registration'), registrationName(d.sip_registration)),
			row(_('Endpoint'), d.sip_endpoint === '1' ? _('Loaded') :
				(d.sip_configured === '1' ? _('Not loaded') : '—'))
		];

		if (d.sip_registration_detail)
			sipRows.push(row(_('Registration detail'),
				E('code', { 'style': 'word-break:break-all' },
					[ d.sip_registration_detail ])));

		var note = sipNote(d);
		if (note)
			sipRows.push(row(_('Note'), note));

		var fxsRows = [
			row(_('FXS device'), d.fxs_device || '/dev/an7581-slic'),
			row(_('PCM channel'), d.pcm_channel || 'PCM0 (channel 0, slot 4)'),
			row(_('Terminal ID'), d.terminal_id || 'AN7581/line1'),
			row(_('Hook'), hookName(d.fxs_hook)),
			row(_('SLIC state'), fxsStateName(d.fxs_state)),
			row(_('PCM device'), d.pcm_present === '1' ? _('Exists') : _('Missing'))
		];

		return E('div', { 'class': 'cbi-map' }, [
			E('div', { 'class': 'cbi-map-descr' },
				_('Show the runtime state of the voice service network, SIP registration and FXS line.')),
			section(_('Voice service network'), networkRows),
			section(_('Voice service'), serviceRows),
			section(_('SIP / PJSIP'), sipRows),
			section(_('FXS line'), fxsRows),
			actionButtons(this)
		]);
	}
});

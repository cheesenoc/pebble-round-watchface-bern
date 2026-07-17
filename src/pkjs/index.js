// PebbleKit JS companion for the Bern Pebble Round watchface.
//
// Fetches:
//  - Air temperature in Bern from Open-Meteo (free, no API key)
//  - Aare river temperature at Bern from the Aare.guru API (free, no API key)
// and sends each to the watch independently via AppMessage as soon as it
// resolves, so a slow/failed request for one doesn't block the other.
//
// Bern's coordinates are hardcoded since this watchface is specifically
// about Bern — no need for geolocation or a "location" capability.

var BERN_LAT = 46.9480;
var BERN_LON = 7.4474;

var AIR_TEMP_URL = 'https://api.open-meteo.com/v1/forecast?latitude=' +
    BERN_LAT + '&longitude=' + BERN_LON + '&current=temperature_2m&timezone=Europe%2FZurich';

var AARE_TEMP_URL = 'https://aareguru.existenz.ch/v2018/current?city=bern&app=pebble-round-watchface-bern&version=1.0.0';

var xhrRequest = function (url, type, onSuccess, onError) {
  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    if (xhr.status >= 200 && xhr.status < 300) {
      onSuccess(xhr.responseText);
    } else {
      onError('HTTP ' + xhr.status);
    }
  };
  xhr.onerror = function () {
    onError('network error');
  };
  xhr.open(type, url);
  xhr.send();
};

function sendToWatch(dictionary) {
  Pebble.sendAppMessage(dictionary,
    function () { console.log('Sent to Pebble: ' + JSON.stringify(dictionary)); },
    function () { console.log('Error sending to Pebble: ' + JSON.stringify(dictionary)); }
  );
}

function fetchAirTemp() {
  xhrRequest(AIR_TEMP_URL, 'GET',
    function (responseText) {
      var json = JSON.parse(responseText);
      var temp = Math.round(json.current.temperature_2m);
      sendToWatch({ 'AIR_TEMP': temp });
    },
    function (err) {
      console.log('Air temp fetch failed: ' + err);
    }
  );
}

function fetchAareTemp() {
  xhrRequest(AARE_TEMP_URL, 'GET',
    function (responseText) {
      var json = JSON.parse(responseText);
      if (!json.aare || typeof json.aare.temperature !== 'number') {
        console.log('Unexpected Aare.guru response: ' + responseText);
        return;
      }
      var temp = Math.round(json.aare.temperature);
      sendToWatch({ 'AARE_TEMP': temp });
    },
    function (err) {
      console.log('Aare temp fetch failed: ' + err);
    }
  );
}

function fetchAll() {
  fetchAirTemp();
  fetchAareTemp();
}

Pebble.addEventListener('ready', function () {
  console.log('PebbleKit JS ready!');
  fetchAll();
});

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload['REQUEST_DATA']) {
    fetchAll();
  }
});

'use strict';

// Runs inside the generated Clay config page (Clay injects it via
// Function.toString, so it must not reference anything outside its own body).
//
// Pressing the "Forget check-offs and reload" button submits the page with the
// current form values plus a one-shot ResetChecks flag, which closes the config
// and makes the phone side drop local check-offs and the cached list, then
// reload from H-E-B. This mirrors what the Save button does on submit.
module.exports = function() {
  var clayConfig = this;
  clayConfig.on(clayConfig.EVENTS.AFTER_BUILD, function() {
    var forgetButton = clayConfig.getItemById('forget-button');
    forgetButton.on('click', function() {
      var payload = clayConfig.serialize();
      payload.ResetChecks = { value: true };
      location.href = (window.returnTo || 'pebblejs://close#') +
                      encodeURIComponent(JSON.stringify(payload));
    });
  });
};
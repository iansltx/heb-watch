'use strict';

// Runs inside the generated Clay config page (Clay injects it via
// Function.toString, so it must not reference anything outside its own body).
//
// Pressing the "Forget check-offs and reload" button arms the ResetChecks flag
// and submits the page right away, which closes the config and makes the phone
// side drop local check-offs and the cached list, then reload from H-E-B.
// If auto-submit is ever unavailable, the armed toggle still applies on Save.
module.exports = function() {
  var clayConfig = this;
  clayConfig.on(clayConfig.EVENTS.AFTER_BUILD, function() {
    var forgetButton = clayConfig.getItemById('forget-button');
    var resetToggle = clayConfig.getItemByMessageKey('ResetChecks');
    forgetButton.on('click', function() {
      resetToggle.set(true);
      var submitButton = document.querySelector('.component-submit button');
      if (submitButton) { submitButton.click(); }
    });
  });
};
const addon = require('bindings')('pcsc'); // Finds the correct addon file

// Directly export the functions from the C++ addon
module.exports = {
  /**
   * Synchronously lists the names of all available PC/SC readers.
   * @returns {string[]} An array of strings containing the reader names.
   * @throws {Error} If the PC/SC context cannot be established or if listing readers fails.
   */
  getAllReaders: addon.getAllReaders,

  /**
   * Starts listening for card insertions on the specified reader.
   * When a card is detected, it sends the default Get UID command.
   * @param {string} readerName - The name of the reader to listen on.
   * @param {(uid: string) => void} onUid - The callback function invoked when a card UID (hex string) is successfully read.
   * @param {(errorMessage: string) => void} onError - The callback function invoked when an error occurs during listening (the error message is passed as a string).
   * @throws {Error} If a synchronous error occurs during initialization (e.g., already listening, invalid parameters, context cannot be established, listener thread failed to start).
   */
  startListening: addon.startListening,

  /**
   * Stops the active card listener.
   * Waits for the background listener thread to terminate.
   */
  stopListening: addon.stopListening,

  /**
   * Sends a raw APDU command to the card in the specified reader and receives the response.
   * This operation is asynchronous and returns a Promise.
   * @param {string} readerName - The name of the reader to send the command to.
   * @param {Buffer} apdu - A Node.js Buffer object containing the raw APDU command to send.
   * @returns {Promise<Buffer>} A Promise that resolves with a Buffer containing the raw APDU response from the card (including the SW1/SW2 status words). The Promise rejects if an error occurs.
   */
  transmit: addon.transmit // The newly added asynchronous transmit function
};

// Optional: Add simple wrappers around the functions
// to, for example, convert the string error from the onError callback into an Error object.
/*
const originalStartListening = addon.startListening;
module.exports.startListening = (readerName, onUid, onError) => {
  if (typeof onError !== 'function') {
    throw new Error('The onError callback function is required.');
  }
  // Call the original function, but wrap the onError callback
  originalStartListening(readerName, onUid, (errMsg) => {
    onError(new Error(errMsg)); // Convert string to Error object
  });
};
*/
// If you want to use this wrapper, uncomment the code above and
// remove or comment out the direct export of addon.startListening.
// For simplicity, we are doing the direct export for now.
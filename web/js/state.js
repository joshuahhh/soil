export { S };

// the page's shared mutable state: what more than one module writes. state
// that only one module writes stays in that module and is exported from it,
// which makes it read-only everywhere else (an import can't be assigned to),
// so S.x reads as "anything may change this" and an imported x as "only its
// own module does"
const S = {
  gain: 1, // the speed (- =; input.js), re-initialized from the saved view once it loads
  // the pointer's look-around, gathered between frames: input.js adds the
  // drag, the frame loop the arrow keys, and the frame (or the follow
  // camera) spends it
  yaw: 0, pitch: 0,
  // the map follows the camera (map.js) until it is grabbed; anything that
  // puts the camera somewhere new turns the following back on
  following: true,
  followVel: null, prevMark: null, // the follow ease's velocity lead
  curl: 0, // the roll, for the tube and the tunnel alike: 0 flat .. 1 closed
  // the tunnel's settings (tunnel.js; the saved view and __demo set them too)
  tunnelC: 2000, // metres around
  tunnelAcross: true, // the roll runs along the heading (else around it)
  tunnelLag: 0, // seconds for the tube to swing round after a turn; 0 at once
  tunnelK: 10, // looking down the pipe: how far it runs each way, in circumferences
  pendingShot: null, // runs at the end of the next frame, once (shots)
  refImageKey: null, // name:size of the stored photo, to spot a re-drop
};

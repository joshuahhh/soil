export { S };

// the page's shared mutable state: what more than one module writes. state
// that only one module writes stays in that module and is exported from it,
// which makes it read-only everywhere else (an import can't be assigned to),
// so S.x reads as "anything may change this" and an imported x as "only its
// own module does"
const S = {
  // view switches (input.js), which the tube, the tunnel, the follow camera,
  // the solver and the saved view's restore force as well
  gain: 1, // re-initialized from the saved view once it loads
  ortho: false,
  hug: false, // terrain hug: hold height above the mesh, not the sphere
  // the pointer's look-around, gathered between frames: input.js adds the
  // drag, the frame loop the arrow keys, and the frame (or the follow
  // camera) spends it
  yaw: 0, pitch: 0,
  // the map follows the camera (map.js) until it is grabbed; anything that
  // puts the camera somewhere new turns the following back on
  following: true,
  followVel: null, prevMark: null, // the follow ease's velocity lead
  // the tube (tube.js). the tunnel is the same machinery and switches it
  // off, and the keys and the saved view's restore set it up
  tubeRect: null, // [latMin, lonMin, latMax, lonMax] degrees
  tubeOn: false,
  tubeBody: false, // B: the rect wrapped onto the body, not rolled
  // which way the tube axis runs; the rolled-up (glued) edges are the two
  // edges parallel to it. toggled by the #axisbtn switch
  tubeAxisEw: true,
  // staged pipeline: the sheet stays flat while the rect's tiles load at
  // full res; once the scene is complete the engine measures ground level
  // off the mesh, and only then does the roll (and camera fly-in) happen.
  // 'fly' also teleports into the tube mouth; 'roll' just rolls in place
  tubePending: null, // null | 'fly' | 'roll'
  curl: 0, // the roll, for the tube and the tunnel alike: 0 flat .. 1 closed
  // the tunnel's settings (tunnel.js; the saved view and __demo set them too)
  tunnelC: 2000, // metres around
  tunnelAcross: true, // the roll runs along the heading (else around it)
  tunnelLag: 0, // seconds for the tube to swing round after a turn; 0 at once
  tunnelK: 10, // looking down the pipe: how far it runs each way, in circumferences
  pendingShot: null, // runs at the end of the next frame, once (shots)
  refImageKey: null, // name:size of the stored photo, to spot a re-drop
};

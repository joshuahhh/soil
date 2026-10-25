export { sunPosition };

// --- the sun ------------------------------------------------------------
// where the sun is, from the clock and the place: noaa's low-precision
// solar position, good to a fraction of a degree. no dom, no engine
const sunPosition = (date, lat, lon) => {
  const rad = Math.PI / 180;
  const jd = date.getTime() / 86400000 + 2440587.5;
  const t = (jd - 2451545) / 36525;
  const L = (280.46646 + t * (36000.76983 + t * 0.0003032)) % 360;
  const M = (357.52911 + t * (35999.05029 - 0.0001537 * t)) * rad;
  const C = Math.sin(M) * (1.914602 - t * (0.004817 + 0.000014 * t)) + Math.sin(2 * M) * (0.019993 - 0.000101 * t) + Math.sin(3 * M) * 0.000289;
  const lam = (L + C - 0.00569 - 0.00478 * Math.sin((125.04 - 1934.136 * t) * rad)) * rad;
  const eps = (23.439291 - 0.0130042 * t) * rad;
  const dec = Math.asin(Math.sin(eps) * Math.sin(lam));
  const ra = Math.atan2(Math.cos(eps) * Math.sin(lam), Math.cos(lam));
  const gmst = (280.46061837 + 360.98564736629 * (jd - 2451545)) % 360;
  const ha = ((gmst + lon) * rad - ra);
  const la = lat * rad;
  const el = Math.asin(Math.sin(la) * Math.sin(dec) + Math.cos(la) * Math.cos(dec) * Math.cos(ha));
  const az = Math.atan2(-Math.sin(ha), Math.cos(la) * Math.tan(dec) - Math.sin(la) * Math.cos(ha));
  return { az: ((az / rad) + 360) % 360, el: el / rad };
};

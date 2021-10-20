const inv = [];
const b64chars =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
for (var i = 0; i < b64chars.length - 1; i++) {
  inv[b64chars.charCodeAt(i) - 43] = i;
}
console.log(
  inv[0],
  inv.map((x) =>
    x === "null" || typeof x === "undefined" || isNaN(x) ? -1 : x
  )
);

return function(s) {
  if (typeof s !== "string") s = String(s);
  return "'" + s.replace(/'/g, "'\''") + "'";
};

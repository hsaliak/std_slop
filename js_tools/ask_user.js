const native_ask_user = tools.ask_user;
return function(args) {
  return native_ask_user(args || {});
};


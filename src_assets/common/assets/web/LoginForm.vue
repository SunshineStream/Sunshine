<template>
  <form id="login-form" @submit.prevent="save" method="post" autocomplete="on">
    <div class="mb-2">
      <label for="usernameInput" class="form-label">Username:</label>
      <input type="text" class="form-control" id="usernameInput" autocomplete="username" name="username"
        v-model="passwordData.username" autofocus/>
    </div>
    <div class="mb-2">
      <label for="passwordInput" class="form-label">Password:</label>
      <input type="password" class="form-control" id="passwordInput" autocomplete="current-password" name="current-password"
        v-model="passwordData.password" required />
    </div>
    <input type="submit" class="btn btn-primary w-100 mb-2" v-bind:disabled="loading" value="Login"/>
    <div class="alert alert-danger" v-if="error"><b>Error: </b>{{ error }}</div>
    <div class="alert alert-success" v-if="success">
      <b>Success! </b>
    </div>
  </form>
</template>

<script>
export default {
  data() {
    return {
      error: null,
      success: false,
      loading: false,
      passwordData: {
        username: "",
        password: ""
      },
    };
  },
  methods: {
    save() {
      this.error = null;
      this.loading = true;
      fetch("/api/login", {
        method: "POST",
        body: JSON.stringify(this.passwordData),
      }).then((r) => {
        this.loading = false;
        if (r.status === 200) {
          r.json().then((rj) => {
            if (rj.status.toString() === "true") {
              this.success = true;
              const emitter = this.$emit;

              if (window.PasswordCredential) {
                const c = new PasswordCredential(
                    document.getElementById("login-form")
                );

                navigator.credentials.store(c).then((a) => {
                  emitter('loggedin');
                }).catch((err) => {
                  emitter('loggedin');
                });
              } else {
                emitter('loggedin');
              }
            } else {
              this.error = rj.error || "Invalid Username or Password";
            }
          });
        } else {
          this.error = "Internal Server Error";
        }
      });
    },
  },
}
</script>

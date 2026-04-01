<script setup>
import { ref } from 'vue'
import { verifySecret } from '../api.js'

const emit = defineEmits(['authenticated'])

const password = ref('')
const error = ref('')
const submitting = ref(false)

async function submit() {
  if (!password.value.trim()) {
    error.value = '请输入密钥'
    return
  }
  submitting.value = true
  error.value = ''

  const result = await verifySecret(password.value.trim())
  submitting.value = false
  if (result.ok) {
    emit('authenticated')
  } else {
    error.value = result.error
  }
}
</script>

<template>
  <div class="auth-overlay">
    <div class="auth-card">
      <div class="auth-header">
        <svg viewBox="0 0 24 24" fill="currentColor" width="24" height="24">
          <path d="M12 17a2 2 0 1 0 0-4 2 2 0 0 0 0 4zm6-9a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V10a2 2 0 0 1 2-2h1V6a5 5 0 1 1 10 0v2h1zm-6-5a3 3 0 0 0-3 3v2h6V6a3 3 0 0 0-3-3z"/>
        </svg>
        <h2>需要验证</h2>
      </div>
      <p class="auth-desc">输入服务器启动时显示的密钥以访问控制界面</p>
      <form @submit.prevent="submit">
        <input
          type="password"
          v-model="password"
          placeholder="密钥"
          :disabled="submitting"
          autofocus
        />
        <div v-if="error" class="auth-error">{{ error }}</div>
        <button type="submit" :disabled="submitting">
          {{ submitting ? '验证中...' : '确认' }}
        </button>
      </form>
    </div>
  </div>
</template>

<style scoped>
.auth-overlay {
  position: fixed;
  inset: 0;
  background: rgba(0, 0, 0, 0.85);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
}
.auth-card {
  background: var(--bg-surface);
  border: 1px solid var(--border-light);
  border-radius: var(--radius);
  padding: 32px;
  max-width: 320px;
  width: 100%;
}
.auth-header {
  display: flex;
  align-items: center;
  gap: 12px;
  margin-bottom: 16px;
  color: var(--accent);
}
.auth-header h2 {
  font-size: 18px;
  font-weight: 600;
}
.auth-header svg {
  width: 24px;
  height: 24px;
}
.auth-desc {
  color: var(--text-secondary);
  font-size: 14px;
  margin-bottom: 20px;
  line-height: 1.5;
}
input {
  width: 100%;
  padding: 12px 14px;
  border: 1px solid var(--border-light);
  border-radius: var(--radius-sm);
  background: var(--bg-elevated);
  color: var(--text-primary);
  font-size: 15px;
  margin-bottom: 12px;
}
input:focus {
  outline: none;
  border-color: var(--accent);
}
input::placeholder {
  color: var(--text-muted);
}
.auth-error {
  color: var(--accent);
  font-size: 13px;
  margin-bottom: 12px;
}
button {
  width: 100%;
  padding: 12px;
  background: var(--accent);
  color: #fff;
  border: none;
  border-radius: var(--radius-sm);
  font-size: 15px;
  font-weight: 500;
  cursor: pointer;
  transition: background 0.2s;
}
button:hover:not(:disabled) {
  background: var(--accent-hover);
}
button:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}
</style>
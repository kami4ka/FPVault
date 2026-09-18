/* SPDX-License-Identifier: GPL-3.0-or-later */
import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { App } from './App.js'
import './theme.css'

const root = document.getElementById('root')
if (!root) throw new Error('#root missing from index.html')

createRoot(root).render(
  <StrictMode>
    <App />
  </StrictMode>
)

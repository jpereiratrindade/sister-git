#!/usr/bin/env python3
import json, os, subprocess, sys, time, urllib.error, urllib.request

base, token, driver = sys.argv[1:4]
proc = subprocess.Popen([driver, "--port", "4444"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def request(method, path, data=None):
    body = json.dumps(data).encode() if data is not None else None
    req = urllib.request.Request("http://127.0.0.1:4444" + path, body, method=method,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req) as response:
            return json.load(response).get("value")
    except urllib.error.HTTPError as error:
        raise RuntimeError(error.read().decode()) from error

def execute(script):
    return request("POST", f"/session/{session}/execute/sync", {"script": script, "args": []})

def execute_async(script):
    return request("POST", f"/session/{session}/execute/async", {"script": script, "args": []})

def wait_js(script, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        if execute(script): return
        time.sleep(.1)
    state = execute("return {url:location.href,text:document.body.innerText.slice(0,3000)}")
    raise AssertionError("timeout: " + script + "\n" + json.dumps(state, ensure_ascii=False))

try:
    for _ in range(50):
        try:
            session = request("POST", "/session", {"capabilities": {"alwaysMatch": {
                "browserName": "firefox", "moz:firefoxOptions": {"args": ["-headless"]}}}})["sessionId"]
            break
        except Exception: time.sleep(.1)
    else: raise RuntimeError("geckodriver não iniciou")

    request("POST", f"/session/{session}/url", {"url": base})
    wait_js("return document.readyState === 'complete'")
    execute(f"localStorage.setItem('sister_token', {json.dumps(token)});")
    request("POST", f"/session/{session}/url", {"url": base})
    wait_js("return typeof window.handleCreateProject === 'function' && document.body.innerText.includes('Novo Artigo')")

    execute("window.openNewProjectModal(); document.querySelector('#newProjName').value='e2e-editorial';"
            "document.querySelector('#newProjTitle').value='Fluxo E2E';"
            "document.querySelector('#newProjAuthor').value='Teste Firefox';"
            "document.querySelector('#newProjDesc').value='Criado pelo navegador';")
    execute_async("const done=arguments[arguments.length-1]; window.handleCreateProject({preventDefault(){}}).then(()=>done(true)).catch(e=>done(String(e)));")
    wait_js("return location.hash.includes('e2e-editorial/editor') && !!document.querySelector('#liveEditorTextarea')")
    execute("let t=document.querySelector('#liveEditorTextarea'); t.value='# Fluxo E2E\\n\\nConteúdo pelo Firefox.';"
            "t.dispatchEvent(new Event('input',{bubbles:true})); document.querySelector('#commitMsg').value='Edição E2E';"
            "document.querySelector('.editor-commit-bar').requestSubmit();")
    wait_js("return document.body.innerText.includes('Versão Salva')")

    execute_async("const done=arguments[arguments.length-1]; window.prompt=()=> 'Submissão E2E'; window.handleEditorialTransition('e2e-editorial','submitted').then(()=>done(true));")
    wait_js("return document.body.innerText.toLowerCase().includes('submetido')")
    execute_async("const done=arguments[arguments.length-1]; window.prompt=()=> 'Revisão E2E'; window.handleEditorialTransition('e2e-editorial','in_review').then(()=>done(true));")
    wait_js("return document.body.innerText.toLowerCase().includes('em revisão')")
    execute("window.openNewReviewModal('e2e-editorial');"
            "document.querySelector('#reviewRecommendation').value='accept';"
            "document.querySelector('#reviewSummary').value='Parecer favorável emitido no Firefox';")
    execute_async("const done=arguments[arguments.length-1]; window.handleCreateReview({preventDefault(){}}).then(()=>done(true));")
    wait_js("return document.body.innerText.includes('Parecer favorável emitido no Firefox')")
    execute_async("const done=arguments[arguments.length-1]; window.prompt=()=> 'Aceite E2E'; window.handleEditorialTransition('e2e-editorial','accepted').then(()=>done(true));")
    wait_js("return document.body.innerText.toLowerCase().includes('aceito para publicação')")

    execute("window.openPublishModal('e2e-editorial'); document.querySelector('#publishTag').value='ed/v1.0';"
            "document.querySelector('#publishTitle').value='Fluxo E2E';"
            "document.querySelector('#publishNotes').value='Publicado pelo Firefox headless';"
            "document.querySelector('#modalPublish form').requestSubmit();")
    wait_js("return document.body.innerText.toLowerCase().includes('publicado') && document.body.innerText.includes('ed/v1.0')")
    request("POST", f"/session/{session}/url", {"url": base + "#/r/e2e-editorial/code"})
    wait_js("return [...document.querySelectorAll('span')].some(e => e.innerText.includes('Último commit:') && !e.innerText.includes('—'))")
finally:
    try: request("DELETE", f"/session/{session}")
    except Exception: pass
    proc.terminate(); proc.wait(timeout=5)

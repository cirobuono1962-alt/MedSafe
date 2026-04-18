export default async function handler(req, res) {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  if (req.method === 'OPTIONS') { res.status(200).end(); return; }

  const q = req.query.q;
  if (!q || q.length < 2) {
    res.status(400).json({ error: 'Query too short', results: [] });
    return;
  }

  try {
    const payload = {
      model: 'claude-haiku-4-5-20251001',
      max_tokens: 1000,
      messages: [{
        role: 'user',
        content: 'Sei un esperto di farmaci italiani autorizzati da AIFA. Cerca il farmaco: ' + q + '. Rispondi SOLO con un array JSON in italiano. Ogni oggetto deve avere: name (nome commerciale italiano con dosaggio), principle (principio attivo in italiano), producer (azienda produttrice), dose (dosaggio), forma (Compressa/Capsula/Fiala/Gocce/Spray/Cerotto/Sciroppo), cat (categoria terapeutica in italiano). Usa nomi italiani. Max 10 risultati. Solo JSON puro senza markdown.'
      }]
    };

    const claudeRes = await fetch('https://api.anthropic.com/v1/messages', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'x-api-key': process.env.ANTHROPIC_API_KEY,
        'anthropic-version': '2023-06-01'
      },
      body: JSON.stringify(payload)
    });

    if (!claudeRes.ok) {
      const err = await claudeRes.text();
      throw new Error('Status ' + claudeRes.status + ': ' + err.substring(0, 200));
    }

    const data = await claudeRes.json();
    const text = data.content[0].text.replace(/```json|```/g, '').trim();
    const results = JSON.parse(text);
    res.status(200).json({ results: Array.isArray(results) ? results : [], source: 'AI' });
  } catch(e) {
    res.status(500).json({ error: e.message, results: [] });
  }
}

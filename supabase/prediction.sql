-- Records what the website's models guessed for each garment, next to what
-- was finally saved, so you can see when (and how) they get it wrong.
--
-- Run once in Supabase: SQL Editor -> New query -> paste this -> Run.
-- Safe to run again: it only creates what doesn't exist yet.
--
-- One row per model per garment:
--   garment_type  Marqo-FashionCLIP's guess          e.g. "Jeans"  -> "Jacket / coat"
--   colour        the colour detection               e.g. "BLU"    -> "NVY"
--   fibres        PaddleOCR + the fibre parser       e.g. "CO 95, EL 5" -> "CO 95, EL 5"
--
-- To look at the results, open these in the Table Editor (or select from them):
--   prediction_mistakes  every wrong guess, newest first
--   prediction_accuracy  % correct per model and model version

-- pgvector, for the garment embeddings (Supabase ships it; this switches it on).
create extension if not exists vector with schema extensions;

create table if not exists public.prediction (
  prediction_id  uuid primary key default gen_random_uuid(),
  clothing_id    uuid not null references public.clothing (clothing_id) on delete cascade,
  model          text not null check (model in ('garment_type', 'colour', 'fibres')),
  -- Changes when the model, its prompts or its settings change, so accuracy
  -- can be compared before and after.
  model_version  text not null,
  -- Both written the same way by the website, so they can be compared as text.
  -- "none": the model had no answer. "unknown": the composition was ticked unknown.
  predicted      text not null,
  final          text not null,
  correct        boolean generated always as (predicted = final) stored,
  -- 0-1, when the model gives one (garment_type does).
  confidence     real check (confidence between 0 and 1),
  -- Extra detail: the runner-up guesses, the raw text read from the label, etc.
  details        jsonb,
  -- garment_type only: the 512 numbers the classifier made from the photo.
  -- With the final garment type (clothing.category_id) this is a labelled
  -- example that a future version of the site can learn from. The photo itself
  -- is never stored.
  embedding      extensions.vector(512),
  created_at     timestamptz not null default now()
);

create index if not exists prediction_clothing_idx on public.prediction (clothing_id);
create index if not exists prediction_model_idx on public.prediction (model, created_at desc);

-- The website (publishable key) may add rows but not read, change or delete
-- them. You see everything in the Supabase dashboard as usual.
alter table public.prediction enable row level security;
grant insert on public.prediction to anon, authenticated;
drop policy if exists "website can record predictions" on public.prediction;
create policy "website can record predictions" on public.prediction
  for insert to anon, authenticated with check (true);

create or replace view public.prediction_mistakes
with (security_invoker = true) as
select created_at, model, predicted, final, confidence, model_version, details, clothing_id
from public.prediction
where not correct
order by created_at desc;

create or replace view public.prediction_accuracy
with (security_invoker = true) as
select model, model_version,
       count(*)                                         as guesses,
       count(*) filter (where correct)                  as correct,
       round(100.0 * avg(correct::int), 1)              as percent_correct,
       min(created_at)                                  as first_seen,
       max(created_at)                                  as last_seen
from public.prediction
group by model, model_version
order by model, last_seen desc;

-- The views are for you in the dashboard, not for the website.
revoke all on public.prediction_mistakes, public.prediction_accuracy from anon, authenticated;

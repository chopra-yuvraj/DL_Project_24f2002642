# DL_Project_24f2002642

Messy Mashup DL & GenAI Project, T1-2026  
IIT Madras BS Data Science | Roll No: 24f2002642

Live demo: [spotiknow.vercel.app](https://spotiknow.vercel.app)

---

## What this project is about

This is my submission for the Messy Mashup Kaggle competition. The task is to classify audio files into one of 10 music genres:

`blues` `classical` `country` `disco` `hiphop` `jazz` `metal` `pop` `reggae` `rock`

The tricky part is that the training data has clean separated stems (drums, vocals, bass, other) but the test data is messy mixed audio. So the model has to generalize across that gap.

---

## Models built

I built 3 different models as required for the viva.

### 1. LightGBM Baseline (Classical ML)
- For each song, the four stems (drums, vocals, bass, other) are mixed together, a random 10-second crop is taken, and then 124 features are extracted 40 MFCCs with mean and std, 12 Chroma coefficients, Spectral Contrast, RMS energy, Zero Crossing Rate, and Spectral Rolloff. The dataset used 80 songs per genre with 3 clips each, giving 2,400 training samples total.
Training used 5-fold cross-validation with LightGBM (learning rate 0.05, num_leaves 63). The CV macro F1 across folds came out to around 0.92, which looks impressive on paper. But the Kaggle score was only 0.22. The reason is straightforward the model learned from clean separated stems, but the test files are mixed mashups that sound completely different.
-  Kaggle score: 0.22

### 2. MelCNN Scratch (Deep Learning from scratch)
- A CNN built entirely from scratch no pretrained weights anywhere. The input is a 128-band mel spectrogram computed from 30-second clips. The model has 5,014,250 parameters. Training data was 4,000 synthetically generated samples per epoch (random stem mixes), validated on 150 held-out songs (15 per genre).
Training ran for 12 epochs with AdamW, MixUp augmentation (alpha=0.4), SpecAugment (TimeMask=80, FreqMask=27), and label smoothing of 0.1. The training curve showed clear learning val F1 went from 0.33 at epoch 1 all the way to 0.864 at epoch 11, with the best checkpoint saved at that point. Final Kaggle score was 0.606. For a model with no pretrained knowledge of audio, this is a solid result and shows the mel spectrogram input pipeline was working well.
- Kaggle score: 0.606

### 3. ResNet50 MelSpec TwoPhase (Transfer Learning)
- Instead of training a CNN from scratch, this model uses ResNet50 pretrained on ImageNet and adapts it for audio by feeding it 3-channel mel spectrograms channel 1 is the mel spectrogram itself, channel 2 is the first order delta (rate of change over time), and channel 3 is the delta-delta (acceleration). This gives the model something closer to how we perceive audio changes, not just a static snapshot. Total parameters: 24,563,274 with 1,055,242 in the custom head.
Training followed a two-phase strategy. Phase 1 froze the backbone and trained only the head (1,055,242 params) for 3 epochs val F1 went from 0.474 → 0.607 → 0.616. Phase 2 unfroze everything and fine-tuned with layer-wise learning rates for 5 more epochs. Val F1 peaked at 0.6611. The model never really broke past 0.66 on validation, which suggests that ImageNet features don't transfer as cleanly to audio as AudioSet-pretrained features do. 
- Kaggle score: 0.559

### 4. AST Two-Phase Finetune (Pretrained Transformer)
-The strongest model by a significant margin. The Audio Spectrogram Transformer (MIT/ast-finetuned-audioset-10-10-0.4593) was originally trained on AudioSet with 527 classes. The classifier head was replaced with a 10-class head for genres, and the full 86,196,490 parameter model was fine-tuned on the competition data.
Training data was 5,000 synthetically mixed samples per epoch   each sample takes 4 stems from songs of the same genre, applies tempo stretching (±12%), random gain, and ESC-50 environmental noise at 70% probability before passing through the ASTFeatureExtractor. The 85/15 train-val split gave 150 clean validation samples.
Phase 1 froze the entire backbone and trained just the 9,226-parameter classifier head for 3 epochs. Val F1 went 0.765 → 0.824 → 0.818, with the best weights saved at epoch 2. Phase 2 loaded those weights, unfroze all 86M parameters, and used layer-wise learning rates (backbone at 2e-5, head 10× higher at 2e-4). Over 5 epochs the val F1 went 0.816 → 0.839 → 0.825 → 0.831 → 0.832, with early stopping triggered after epoch 5. Best Phase 2 checkpoint was at epoch 2 (val F1: 0.8388).

- Kaggle score: 0.8911

---

## Results summary

| Model | Val F1 | Kaggle F1 |
|---|---|---|
| LightGBM Baseline | 0.919| 0.220 |
| MelCNN Scratch | 0.864 | 0.606 |
| ResNet50 TwoPhase | 0.661 | 0.559 |
| AST TwoPhase | **0.839** | **0.891** |

---

## W&B Tracking

All runs are logged at: [wandb.ai/choprayuvraj-iit-madras/24f2002642-t12026](https://wandb.ai/choprayuvraj-iit-madras/24f2002642-t12026)

Report at:
[https://api.wandb.ai/links/choprayuvraj-iit-madras/inmyz547](https://api.wandb.ai/links/choprayuvraj-iit-madras/inmyz547)


Metrics tracked: `train_loss`, `train_f1`, `train_acc`, `val_loss`, `val_f1`, `val_acc`, `lr`

---

## Deployment

The AST model is deployed on HuggingFace Spaces (Gradio) and redirected through Vercel.

- HuggingFace Space: [Yuvraj-Chopra/DL-GEN-AI-Project](https://huggingface.co/spaces/Yuvraj-Chopra/DL-GEN-AI-Project)

- Live URL: [spotiknow.vercel.app](https://spotiknow.vercel.app)

Upload any WAV or MP3 file and it returns the predicted genre.

---

## Files in this repo

| File | Description |
|---|---|
| `DL-24f2002642-notebook-t12026` | Main AST notebook (primary submission) |
| `vercel.json` | Vercel redirect config for deployment |
| `README.md` | This file |

The LightGBM, MelCNN, and ResNet50 notebooks are on Kaggle under the same notebook name with different versions.

---

## Competition

Kaggle competition: Messy Mashup (jan-2026-dl-gen-ai-project)  
Final submission score: **0.89108**  
Evaluation metric: Macro F1 Score
